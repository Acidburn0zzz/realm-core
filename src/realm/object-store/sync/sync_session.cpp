////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include <realm/object-store/sync/sync_session.hpp>

#include <realm/object-store/sync/impl/sync_client.hpp>
#include <realm/object-store/sync/impl/sync_file.hpp>
#include <realm/object-store/sync/impl/sync_metadata.hpp>
#include <realm/object-store/sync/sync_manager.hpp>
#include <realm/object-store/sync/sync_user.hpp>
#include <realm/object-store/sync/app.hpp>

#include <realm/sync/client.hpp>
#include <realm/db_options.hpp>
#include <realm/sync/protocol.hpp>
#include <realm/util/websocket.hpp>

#include <realm/object-store/impl/realm_coordinator.hpp>

using namespace realm;
using namespace realm::_impl;
using namespace realm::_impl::sync_session_states;

using SessionWaiterPointer = void (sync::Session::*)(std::function<void(std::error_code)>);

constexpr const char SyncError::c_original_file_path_key[];
constexpr const char SyncError::c_recovery_file_path_key[];

/// A state which a `SyncSession` can currently be within. State classes handle various actions
/// and state transitions.
///
/// STATES:
///
/// WAITING_FOR_ACCESS_TOKEN: a request has been initiated to ask
/// for an updated access token and the session is waiting for a response.
/// From: ACTIVE
/// To:
///    * ACTIVE: when the SDK successfully refreshes the token
///    * INACTIVE: if asked to log out, or if asked to close
///
/// ACTIVE: the session is connected to the Sync Server and is actively
/// transferring data.
/// From: INACTIVE, DYING, WAITING_FOR_ACCESS_TOKEN
/// To:
///    * INACTIVE: if asked to log out, or if asked to close and the stop policy
///                is Immediate.
///    * DYING: if asked to close and the stop policy is AfterChangesUploaded
///    * WAITING_FOR_ACCESS_TOKEN: if the session tried to enter ACTIVE,
///                                but the token is invalid or expired.
///
/// DYING: the session is performing clean-up work in preparation to be destroyed.
/// From: ACTIVE
/// To:
///    * INACTIVE: when the clean-up work completes, if the session wasn't
///                revived, or if explicitly asked to log out before the
///                clean-up work begins
///    * ACTIVE: if the session is revived
///
/// INACTIVE: the user owning this session has logged out, the `sync::Session`
/// owned by this session is destroyed, and the session is quiescent.
/// Note that a session briefly enters this state before being destroyed, but
/// it can also enter this state and stay there if the user has been logged out.
/// From: initial, ACTIVE, DYING, WAITING_FOR_ACCESS_TOKEN
/// To:
///    * ACTIVE: if the session is revived
///
struct SyncSession::State {
    virtual ~State() {}

    // Move the given session into this state. All state transitions MUST be carried out through this method.
    virtual void enter_state(std::unique_lock<std::mutex>&, SyncSession&) const {}

    virtual void nonsync_transact_notify(std::unique_lock<std::mutex>&, SyncSession&, sync::version_type) const {}

    // Perform any work needed to reactivate a session that is not already active.
    // Returns true iff the session should ask the binding to get a token for `bind()`.
    virtual bool revive_if_needed(std::unique_lock<std::mutex>&, SyncSession&) const
    {
        return false;
    }

    // Perform any work needed to respond to the application regaining network connectivity.
    virtual void handle_reconnect(std::unique_lock<std::mutex>&, SyncSession&) const {}

    // The user that owns this session has been logged out, and the session should take appropriate action.
    virtual void log_out(std::unique_lock<std::mutex>&, SyncSession&) const {}

    // The session should be closed and moved to `inactive`, in accordance with its stop policy and other state.
    virtual void close(std::unique_lock<std::mutex>&, SyncSession&) const {}

    // If the error is fatal advance the state to inactive.
    virtual void handle_error(std::unique_lock<std::mutex>&, SyncSession&, const SyncError&) const {}

    // Transition immediately to `inactive` state. Calling this function must gurantee that any
    // sync::Session object in SyncSession::m_session that existed prior to the time of invocation
    // must have been destroyed upon return. This allows the caller to follow up with a call to
    // sync::Client::wait_for_session_terminations_or_client_stopped() in order to wait for the
    // Realm file to be closed. This works so long as this SyncSession object remains in the
    // `inactive` state after the invocation of shutdown_and_wait().
    virtual void shutdown_and_wait(std::unique_lock<std::mutex>&, SyncSession&) const {}

    // The user's access token was refreshed.
    virtual void access_token_updated(std::unique_lock<std::mutex>&, SyncSession&, std::string) const {}

    static const State& active;
    static const State& dying;
    static const State& inactive;
    static const State& waiting_for_access_token;
};

struct sync_session_states::Active : public SyncSession::State {
    // msvc needs this to initialize the class correctly
    constexpr Active() {}

    void enter_state(std::unique_lock<std::mutex>& lock, SyncSession& session) const override
    {
        auto user = session.user();
        if (user && user->access_token_refresh_required()) {
            session.advance_state(lock, waiting_for_access_token);
            // Release the lock for SDKs with a single threaded
            // networking implementation such as our test suite
            // so that the update can trigger a state change from
            // the completion handler.
            REALM_ASSERT(lock.owns_lock());
            lock.unlock();
            session.initiate_access_token_refresh();
            return;
        }

        // when entering from the Dying state the session will still be bound
        if (!session.m_session) {
            session.create_sync_session();
            session.m_session->bind();
        }

        // Register all the pending wait-for-completion blocks. This can
        // potentially add a redundant callback if we're coming from the Dying
        // state, but that's okay (we won't call the user callbacks twice).
        SyncSession::CompletionCallbacks callbacks_to_register;
        std::swap(session.m_completion_callbacks, callbacks_to_register);

        for (auto& [id, callback_tuple] : callbacks_to_register) {
            session.add_completion_callback(lock, std::move(callback_tuple.second), callback_tuple.first);
        }
    }

    void log_out(std::unique_lock<std::mutex>& lock, SyncSession& session) const override
    {
        session.advance_state(lock, inactive);
    }

    void nonsync_transact_notify(std::unique_lock<std::mutex>&, SyncSession& session,
                                 sync::version_type version) const override
    {
        // Fully ready sync session, notify immediately.
        session.m_session->nonsync_transact_notify(version);
    }

    void close(std::unique_lock<std::mutex>& lock, SyncSession& session) const override
    {
        switch (session.m_config.stop_policy) {
            case SyncSessionStopPolicy::Immediately:
                session.advance_state(lock, inactive);
                break;
            case SyncSessionStopPolicy::LiveIndefinitely:
                // Don't do anything; session lives forever.
                break;
            case SyncSessionStopPolicy::AfterChangesUploaded:
                // Wait for all pending changes to upload.
                session.advance_state(lock, dying);
                break;
        }
    }

    void access_token_updated(std::unique_lock<std::mutex>&, SyncSession& session,
                              std::string signed_access_token) const override
    {
        if (session.m_session) {
            session.m_session->refresh(signed_access_token);
        }
    }

    void shutdown_and_wait(std::unique_lock<std::mutex>& lock, SyncSession& session) const override
    {
        session.advance_state(lock, inactive);
    }

    void handle_reconnect(std::unique_lock<std::mutex>&, SyncSession& session) const override
    {
        session.m_session->cancel_reconnect_delay();
    }
};

struct sync_session_states::Dying : public SyncSession::State {
    // msvc needs this to initialize the class correctly
    constexpr Dying() {}

    void enter_state(std::unique_lock<std::mutex>& lock, SyncSession& session) const override
    {
        // If we have no session, we cannot possibly upload anything.
        if (!session.m_session) {
            session.advance_state(lock, inactive);
            return;
        }

        size_t current_death_count = ++session.m_death_count;
        auto weak_session = session.weak_from_this();
        session.m_session->async_wait_for_upload_completion([weak_session, current_death_count](std::error_code) {
            if (auto session = weak_session.lock()) {
                std::unique_lock<std::mutex> lock(session->m_state_mutex);
                if (session->m_state == &State::dying && session->m_death_count == current_death_count) {
                    session->advance_state(lock, inactive);
                }
            }
        });
    }

    void handle_error(std::unique_lock<std::mutex>& lock, SyncSession& session, const SyncError& error) const override
    {
        if (error.is_fatal) {
            session.advance_state(lock, inactive);
        }
    }

    bool revive_if_needed(std::unique_lock<std::mutex>& lock, SyncSession& session) const override
    {
        // Revive.
        session.advance_state(lock, active);
        return false;
    }

    void access_token_updated(std::unique_lock<std::mutex>&, SyncSession& session,
                              std::string signed_access_token) const override
    {
        if (session.m_session) {
            session.m_session->refresh(signed_access_token);
        }
    }

    void log_out(std::unique_lock<std::mutex>& lock, SyncSession& session) const override
    {
        session.advance_state(lock, inactive);
    }

    void shutdown_and_wait(std::unique_lock<std::mutex>& lock, SyncSession& session) const override
    {
        session.advance_state(lock, inactive);
    }
};

struct sync_session_states::Inactive : public SyncSession::State {
    // msvc needs this to initialize the class correctly
    constexpr Inactive() {}

    void enter_state(std::unique_lock<std::mutex>& lock, SyncSession& session) const override
    {
        // Manually set the disconnected state. Sync would also do this, but
        // since the underlying SyncSession object already have been destroyed,
        // we are not able to get the callback.
        auto old_state = session.m_connection_state;
        auto new_state = session.m_connection_state = SyncSession::ConnectionState::Disconnected;

        SyncSession::CompletionCallbacks waits;
        std::swap(waits, session.m_completion_callbacks);

        session.m_session = nullptr;
        session.unregister(lock); // releases lock

        // Send notifications after releasing the lock to prevent deadlocks in the callback.
        if (old_state != new_state) {
            session.m_connection_change_notifier.invoke_callbacks(old_state, session.connection_state());
        }

        // Inform any queued-up completion handlers that they were cancelled.
        for (auto& [id, callback] : waits)
            callback.second(make_error_code(util::error::operation_aborted));
    }

    void access_token_updated(std::unique_lock<std::mutex>&, SyncSession& session,
                              std::string signed_access_token) const override
    {
        if (session.m_session) {
            session.m_session->refresh(signed_access_token);
        }
    }

    bool revive_if_needed(std::unique_lock<std::mutex>& lock, SyncSession& session) const override
    {
        session.advance_state(lock, active);
        return true;
    }

    void close(std::unique_lock<std::mutex>& lock, SyncSession& session) const override
    {
        session.unregister(lock); // releases lock
    }
};

struct sync_session_states::WaitingForAccessToken : public SyncSession::State {
    // msvc needs this to initialize the class correctly
    constexpr WaitingForAccessToken() {}

    void enter_state(std::unique_lock<std::mutex>&, SyncSession&) const override {}

    void access_token_updated(std::unique_lock<std::mutex>& lock, SyncSession& session,
                              std::string signed_access_token) const override
    {
        // We don't expect there to be a session at this point, but if there is refresh the token there.
        // If not, the latest token will be seeded from SyncUser::access_token() on session creation.
        if (session.m_session) {
            session.m_session->refresh(signed_access_token);
        }
        session.advance_state(lock, active);
    }

    void nonsync_transact_notify(std::unique_lock<std::mutex>&, SyncSession& session,
                                 sync::version_type version) const override
    {
        if (session.m_session) {
            session.m_session->nonsync_transact_notify(version);
        }
    }

    void log_out(std::unique_lock<std::mutex>& lock, SyncSession& session) const override
    {
        session.advance_state(lock, inactive);
    }

    bool revive_if_needed(std::unique_lock<std::mutex>&, SyncSession&) const override
    {
        return false;
    }

    void close(std::unique_lock<std::mutex>& lock, SyncSession& session) const override
    {
        // Immediately kill the session.
        session.advance_state(lock, inactive);
    }
};

const SyncSession::State& SyncSession::State::active = Active();
const SyncSession::State& SyncSession::State::dying = Dying();
const SyncSession::State& SyncSession::State::inactive = Inactive();
const SyncSession::State& SyncSession::State::waiting_for_access_token = WaitingForAccessToken();

std::function<void(util::Optional<app::AppError>)> SyncSession::handle_refresh(std::shared_ptr<SyncSession> session)
{
    return [session](util::Optional<app::AppError> error) {
        using namespace std::chrono;

        auto session_user = session->user();
        auto is_user_expired =
            session_user && session_user->refresh_jwt().expires_at <
                                duration_cast<seconds>(system_clock::now().time_since_epoch()).count();

        if (!session_user) {
            std::unique_lock<std::mutex> lock(session->m_state_mutex);
            session->cancel_pending_waits(lock, error ? error->error_code : std::error_code());
        }
        else if (is_user_expired) {
            std::unique_lock<std::mutex> lock(session->m_state_mutex);
            session->cancel_pending_waits(lock, error ? error->error_code : std::error_code());
            if (session->m_config.error_handler) {
                session->m_config.error_handler(session, SyncError(realm::sync::ProtocolError::bad_authentication,
                                                                   "expired refresh token", true));
            }
        }
        else if (error) {
            if (error->error_code == app::make_client_error_code(app::ClientErrorCode::app_deallocated)) {
                return;
            }
            else if (error->http_status_code &&
                     (*error->http_status_code == 401 || *error->http_status_code == 403)) {
                // A 401 response on a refresh request means that the token cannot be refreshed and we should not
                // retry. This can be because an admin has revoked this user's sessions or the user has been disabled.
                // TODO: ideally this would write to the logs as well in case users didn't set up their error handler.
                std::unique_lock<std::mutex> lock(session->m_state_mutex);
                session->cancel_pending_waits(lock, error->error_code);
                if (session_user && session_user->is_logged_in()) {
                    session_user->log_out();
                }
                if (session->m_config.error_handler) {
                    auto user_facing_error = SyncError(realm::sync::ProtocolError::permission_denied,
                                                       "Unable to refresh the user access token.", true);
                    session->m_config.error_handler(session, user_facing_error);
                }
            }
            else {
                // 10 seconds is arbitrary, but it is to not swamp the server
                std::this_thread::sleep_for(milliseconds(10000));
                if (session_user) {
                    session_user->refresh_custom_data(handle_refresh(session));
                }
            }
        }
        else {
            session->update_access_token(session_user->access_token());
        }
    };
}

SyncSession::SyncSession(SyncClient& client, std::string realm_path, SyncConfig config, bool force_client_resync,
                         SyncManager* sync_manager)
    : m_state(&State::inactive)
    , m_config(std::move(config))
    , m_force_client_resync(force_client_resync)
    , m_realm_path(std::move(realm_path))
    , m_client(client)
    , m_sync_manager(sync_manager)
{
}

std::shared_ptr<SyncManager> SyncSession::sync_manager() const
{
    std::lock_guard<std::mutex> lk(m_state_mutex);
    REALM_ASSERT(m_sync_manager);
    return m_sync_manager->shared_from_this();
}

void SyncSession::detach_from_sync_manager()
{
    shutdown_and_wait();
    std::lock_guard<std::mutex> lk(m_state_mutex);
    m_sync_manager = nullptr;
}

std::string SyncSession::get_recovery_file_path()
{
    return util::reserve_unique_file_name(m_sync_manager->recovery_directory_path(m_config.recovery_directory),
                                          util::create_timestamped_template("recovered_realm"));
}

void SyncSession::update_error_and_mark_file_for_deletion(SyncError& error, ShouldBackup should_backup)
{
    // Add a SyncFileActionMetadata marking the Realm as needing to be deleted.
    std::string recovery_path;
    auto original_path = path();
    error.user_info[SyncError::c_original_file_path_key] = original_path;
    if (should_backup == ShouldBackup::yes) {
        recovery_path = get_recovery_file_path();
        error.user_info[SyncError::c_recovery_file_path_key] = recovery_path;
    }
    using Action = SyncFileActionMetadata::Action;
    auto action = should_backup == ShouldBackup::yes ? Action::BackUpThenDeleteRealm : Action::DeleteRealm;
    m_sync_manager->perform_metadata_update([this, action, original_path = std::move(original_path),
                                             recovery_path = std::move(recovery_path)](const auto& manager) {
        std::string partition_value = m_config.partition_value;
        manager.make_file_action_metadata(original_path, partition_value, m_config.user->identity(), action,
                                          recovery_path);
    });
}

// This method should only be called from within the error handler callback registered upon the underlying
// `m_session`.
void SyncSession::handle_error(SyncError error)
{
    enum class NextStateAfterError { none, inactive, error };
    auto next_state = error.is_fatal ? NextStateAfterError::error : NextStateAfterError::none;
    auto error_code = error.error_code;

    {
        // See if the current state wishes to take responsibility for handling the error.
        std::unique_lock<std::mutex> lock(m_state_mutex);
        m_state->handle_error(lock, *this, error);
    }

    if (error.is_client_reset_requested()) {
        switch (m_config.client_resync_mode) {
            case ClientResyncMode::Manual:
                break;
            case ClientResyncMode::DiscardLocal:
            case ClientResyncMode::Recover: {
                // Performing a client resync requires tearing down our current
                // sync session and creating a new one with a forced client
                // reset. This will result in session completion handlers firing
                // when the old session is torn down, which we don't want as this
                // is supposed to be transparent to the user.
                //
                // To avoid this, we need to move the completion handlers aside temporarily so
                // that moving to the inactive state doesn't clear them - they will be re-registered
                // when the session becomes active again.
                {
                    std::unique_lock<std::mutex> lock(m_state_mutex);
                    m_force_client_resync = true;

                    CompletionCallbacks callbacks;
                    std::swap(m_completion_callbacks, callbacks);
                    advance_state(lock, State::inactive);

                    // FIXME This should be done in a scope guard so that we always do this, even if advance_state
                    // throws.
                    std::swap(callbacks, m_completion_callbacks);
                }
                revive_if_needed();
                return;
            }
        }
    }

    if (error_code.category() == realm::sync::protocol_error_category()) {
        using ProtocolError = realm::sync::ProtocolError;
        switch (static_cast<ProtocolError>(error_code.value())) {
            // Connection level errors
            case ProtocolError::connection_closed:
            case ProtocolError::other_error:
                // Not real errors, don't need to be reported to the binding.
                return;
            case ProtocolError::unknown_message:
            case ProtocolError::bad_syntax:
            case ProtocolError::limits_exceeded:
            case ProtocolError::wrong_protocol_version:
            case ProtocolError::bad_session_ident:
            case ProtocolError::reuse_of_session_ident:
            case ProtocolError::bound_in_other_session:
            case ProtocolError::bad_message_order:
            case ProtocolError::bad_client_version:
            case ProtocolError::illegal_realm_path:
            case ProtocolError::no_such_realm:
            case ProtocolError::bad_changeset:
            case ProtocolError::bad_changeset_header_syntax:
            case ProtocolError::bad_changeset_size:
            case ProtocolError::bad_changesets:
            case ProtocolError::bad_decompression:
            case ProtocolError::unsupported_session_feature:
            case ProtocolError::transact_before_upload:
            case ProtocolError::partial_sync_disabled:
            case ProtocolError::user_mismatch:
            case ProtocolError::too_many_sessions:
                break;
            // Session errors
            case ProtocolError::session_closed:
            case ProtocolError::other_session_error:
            case ProtocolError::disabled_session:
                // The binding doesn't need to be aware of these because they are strictly informational, and do not
                // represent actual errors.
                return;
            case ProtocolError::token_expired: {
                REALM_ASSERT_RELEASE(false);
            }
            case ProtocolError::bad_authentication: {
                std::shared_ptr<SyncUser> user_to_invalidate;
                next_state = NextStateAfterError::none;
                {
                    std::unique_lock<std::mutex> lock(m_state_mutex);
                    user_to_invalidate = user();
                    cancel_pending_waits(lock, error.error_code);
                }
                if (user_to_invalidate)
                    user_to_invalidate->invalidate();
                break;
            }
            case ProtocolError::permission_denied: {
                next_state = NextStateAfterError::inactive;
                update_error_and_mark_file_for_deletion(error, ShouldBackup::no);
                break;
            }
            case ProtocolError::bad_client_file:
            case ProtocolError::bad_client_file_ident:
            case ProtocolError::bad_origin_file_ident:
            case ProtocolError::bad_server_file_ident:
            case ProtocolError::bad_server_version:
            case ProtocolError::client_file_blacklisted:
            case ProtocolError::diverging_histories:
            case ProtocolError::server_file_deleted:
            case ProtocolError::user_blacklisted:
            case ProtocolError::client_file_expired:
            case ProtocolError::invalid_schema_change:
                next_state = NextStateAfterError::inactive;
                update_error_and_mark_file_for_deletion(error, ShouldBackup::yes);
                break;
        }
    }
    else if (error_code.category() == realm::sync::client_error_category()) {
        using ClientError = realm::sync::Client::Error;
        switch (static_cast<ClientError>(error_code.value())) {
            case ClientError::connection_closed:
            case ClientError::pong_timeout:
                // Not real errors, don't need to be reported to the binding.
                return;
            case ClientError::bad_changeset:
            case ClientError::bad_changeset_header_syntax:
            case ClientError::bad_changeset_size:
            case ClientError::bad_client_file_ident:
            case ClientError::bad_client_file_ident_salt:
            case ClientError::bad_client_version:
            case ClientError::bad_compression:
            case ClientError::bad_error_code:
            case ClientError::bad_file_ident:
            case ClientError::bad_message_order:
            case ClientError::bad_origin_file_ident:
            case ClientError::bad_progress:
            case ClientError::bad_protocol_from_server:
            case ClientError::bad_request_ident:
            case ClientError::bad_server_version:
            case ClientError::bad_session_ident:
            case ClientError::bad_state_message:
            case ClientError::bad_syntax:
            case ClientError::bad_timestamp:
            case ClientError::client_too_new_for_server:
            case ClientError::client_too_old_for_server:
            case ClientError::connect_timeout:
            case ClientError::limits_exceeded:
            case ClientError::protocol_mismatch:
            case ClientError::ssl_server_cert_rejected:
            case ClientError::missing_protocol_feature:
            case ClientError::unknown_message:
            case ClientError::http_tunnel_failed:
                // Don't do anything special for these errors.
                // Future functionality may require special-case handling for existing
                // errors, or newly introduced error codes.
                break;
        }
    }
    else {
        // The server replies with '401: unauthorized' if the access token is invalid, expired, revoked, or the user
        // is disabled. In this scenario we attempt an automatic token refresh and if that succeeds continue as
        // normal. If the refresh request also fails with 401 then we need to stop retrying and pass along the error;
        // see handle_refresh().
        if (error_code == util::websocket::make_error_code(util::websocket::Error::bad_response_401_unauthorized)) {
            if (auto u = user()) {
                u->refresh_custom_data(handle_refresh(shared_from_this()));
                return;
            }
        }
        // Unrecognized error code.
        error.is_unrecognized_by_client = true;
    }

    // Dont't bother invoking m_config.error_handler if the sync is inactive.
    // It does not make sense to call the handler when the session is closed.
    if (m_state == &State::inactive) {
        return;
    }

    switch (next_state) {
        case NextStateAfterError::none:
            if (m_config.cancel_waits_on_nonfatal_error) {
                std::unique_lock<std::mutex> lock(m_state_mutex);
                cancel_pending_waits(lock, error.error_code);
            }
            break;
        case NextStateAfterError::inactive: {
            if (error.is_client_reset_requested()) {
                std::unique_lock<std::mutex> lock(m_state_mutex);
                cancel_pending_waits(lock, error.error_code);
            }

            {
                std::unique_lock<std::mutex> lock(m_state_mutex);
                advance_state(lock, State::inactive);
            }
            break;
        }
        case NextStateAfterError::error: {
            std::unique_lock<std::mutex> lock(m_state_mutex);
            cancel_pending_waits(lock, error.error_code);
            break;
        }
    }
    if (m_config.error_handler) {
        m_config.error_handler(shared_from_this(), std::move(error));
    }
}

void SyncSession::cancel_pending_waits(std::unique_lock<std::mutex>& lock, std::error_code error)
{

    CompletionCallbacks callbacks;
    std::swap(callbacks, m_completion_callbacks);
    lock.unlock();

    // Inform any queued-up completion handlers that they were cancelled.
    for (auto& [id, callback] : callbacks)
        callback.second(error);
}

void SyncSession::handle_progress_update(uint64_t downloaded, uint64_t downloadable, uint64_t uploaded,
                                         uint64_t uploadable, uint64_t download_version, uint64_t snapshot_version)
{
    m_progress_notifier.update(downloaded, downloadable, uploaded, uploadable, download_version, snapshot_version);
}

void SyncSession::create_sync_session()
{
    if (m_session)
        return;

    REALM_ASSERT(m_config.user);

    do_create_sync_session();
}

void SyncSession::do_create_sync_session()
{
    sync::Session::Config session_config;
    session_config.signed_user_token = m_config.user->access_token();
    session_config.realm_identifier = m_config.partition_value;
    session_config.changeset_cooker = m_config.transformer;
    session_config.encryption_key = m_config.realm_encryption_key;
    session_config.verify_servers_ssl_certificate = m_config.client_validate_ssl;
    session_config.ssl_trust_certificate_path = m_config.ssl_trust_certificate_path;
    session_config.ssl_verify_callback = m_config.ssl_verify_callback;
    session_config.proxy_config = m_config.proxy_config;
    session_config.multiplex_ident = m_multiplex_identity;
    {
        std::string sync_route = m_sync_manager->sync_route();

        if (!m_client.decompose_server_url(sync_route, session_config.protocol_envelope,
                                           session_config.server_address, session_config.server_port,
                                           session_config.service_identifier)) {
            throw sync::BadServerUrl();
        }
        // FIXME: Java needs the fully resolved URL for proxy support, but we also need it before
        // the session is created. How to resolve this?
        m_server_url = sync_route;
    }

    if (m_config.authorization_header_name) {
        session_config.authorization_header_name = *m_config.authorization_header_name;
    }
    session_config.custom_http_headers = m_config.custom_http_headers;

    if (m_force_client_resync) {
        std::string metadata_dir = m_realm_path + ".resync";
        util::try_make_dir(metadata_dir);

        sync::Session::Config::ClientReset config;
        config.metadata_dir = metadata_dir;
        if (m_config.client_resync_mode != ClientResyncMode::Recover)
            config.recover_local_changes = false;
        session_config.client_reset_config = config;
    }

    m_session = m_client.make_session(m_realm_path, std::move(session_config));

    std::weak_ptr<SyncSession> weak_self = shared_from_this();

    // Configure the sync transaction callback.
    auto wrapped_callback = [this, weak_self](VersionID old_version, VersionID new_version) {
        if (auto self = weak_self.lock()) {
            std::lock_guard l(m_state_mutex);
            if (m_sync_transact_callback) {
                m_sync_transact_callback(old_version, new_version);
            }
        }
    };
    m_session->set_sync_transact_callback(std::move(wrapped_callback));

    // Set up the wrapped progress handler callback
    m_session->set_progress_handler([weak_self](uint_fast64_t downloaded, uint_fast64_t downloadable,
                                                uint_fast64_t uploaded, uint_fast64_t uploadable,
                                                uint_fast64_t progress_version, uint_fast64_t snapshot_version) {
        if (auto self = weak_self.lock()) {
            self->handle_progress_update(downloaded, downloadable, uploaded, uploadable, progress_version,
                                         snapshot_version);
        }
    });

    // Sets up the connection state listener. This callback is used for both reporting errors as well as changes to
    // the connection state.
    m_session->set_connection_state_change_listener([weak_self](sync::Session::ConnectionState state,
                                                                const sync::Session::ErrorInfo* error) {
        // If the OS SyncSession object is destroyed, we ignore any events from the underlying Session as there is
        // nothing useful we can do with them.
        if (auto self = weak_self.lock()) {
            std::unique_lock<std::mutex> lock(self->m_state_mutex);
            auto old_state = self->m_connection_state;
            using cs = sync::Session::ConnectionState;
            switch (state) {
                case cs::disconnected:
                    self->m_connection_state = ConnectionState::Disconnected;
                    break;
                case cs::connecting:
                    self->m_connection_state = ConnectionState::Connecting;
                    break;
                case cs::connected:
                    self->m_connection_state = ConnectionState::Connected;
                    break;
                default:
                    REALM_UNREACHABLE();
            }
            auto new_state = self->m_connection_state;
            lock.unlock();
            self->m_connection_change_notifier.invoke_callbacks(old_state, new_state);
            if (error) {
                self->handle_error(SyncError{error->error_code, std::move(error->detailed_message), error->is_fatal});
            }
        }
    });
}

void SyncSession::set_sync_transact_callback(std::function<sync::Session::SyncTransactCallback> callback)
{
    std::lock_guard l(m_state_mutex);
    m_sync_transact_callback = std::move(callback);
}

void SyncSession::advance_state(std::unique_lock<std::mutex>& lock, const State& state)
{
    REALM_ASSERT(lock.owns_lock());
    REALM_ASSERT(&state != m_state);
    m_state = &state;
    m_state->enter_state(lock, *this);
}

void SyncSession::nonsync_transact_notify(sync::version_type version)
{
    m_progress_notifier.set_local_version(version);

    std::unique_lock<std::mutex> lock(m_state_mutex);
    m_state->nonsync_transact_notify(lock, *this, version);
}

void SyncSession::revive_if_needed()
{
    std::unique_lock<std::mutex> lock(m_state_mutex);
    m_state->revive_if_needed(lock, *this);
}

void SyncSession::handle_reconnect()
{
    std::unique_lock<std::mutex> lock(m_state_mutex);
    m_state->handle_reconnect(lock, *this);
}

void SyncSession::log_out()
{
    std::unique_lock<std::mutex> lock(m_state_mutex);
    m_state->log_out(lock, *this);
}

void SyncSession::close()
{
    std::unique_lock<std::mutex> lock(m_state_mutex);
    m_state->close(lock, *this);
}

void SyncSession::shutdown_and_wait()
{
    {
        std::unique_lock<std::mutex> lock(m_state_mutex);
        m_state->shutdown_and_wait(lock, *this);
    }
    m_client.wait_for_session_terminations();
}

void SyncSession::update_access_token(std::string signed_token)
{
    std::unique_lock<std::mutex> lock(m_state_mutex);
    m_state->access_token_updated(lock, *this, signed_token);
}

void SyncSession::initiate_access_token_refresh()
{
    auto session_user = user();
    if (session_user) {
        session_user->refresh_custom_data(handle_refresh(shared_from_this()));
    }
}

void SyncSession::unregister(std::unique_lock<std::mutex>& lock)
{
    REALM_ASSERT(lock.owns_lock());
    REALM_ASSERT(m_state == &State::inactive); // Must stop an active session before unregistering.

    lock.unlock();
    m_sync_manager->unregister_session(m_realm_path);
}

void SyncSession::add_completion_callback(const std::unique_lock<std::mutex>&,
                                          std::function<void(std::error_code)> callback,
                                          _impl::SyncProgressNotifier::NotifierType direction)
{
    bool is_download = (direction == _impl::SyncProgressNotifier::NotifierType::download);

    m_completion_request_counter++;
    m_completion_callbacks.emplace_hint(m_completion_callbacks.end(), m_completion_request_counter,
                                        std::make_pair(direction, std::move(callback)));
    // If the state is inactive then just store the callback and return. The callback will get
    // re-registered with the underlying session if/when the session ever becomes active again.
    if (!m_session) {
        return;
    }

    auto waiter = is_download ? &sync::Session::async_wait_for_download_completion
                              : &sync::Session::async_wait_for_upload_completion;

    (m_session.get()->*waiter)([weak_self = weak_from_this(), id = m_completion_request_counter](std::error_code ec) {
        auto self = weak_self.lock();
        if (!self)
            return;
        std::unique_lock<std::mutex> lock(self->m_state_mutex);
        auto callback_node = self->m_completion_callbacks.extract(id);
        lock.unlock();
        if (callback_node)
            callback_node.mapped().second(ec);
    });
}

void SyncSession::wait_for_upload_completion(std::function<void(std::error_code)> callback)
{
    std::unique_lock<std::mutex> lock(m_state_mutex);
    add_completion_callback(lock, std::move(callback), _impl::SyncProgressNotifier::NotifierType::upload);
}

void SyncSession::wait_for_download_completion(std::function<void(std::error_code)> callback)
{
    std::unique_lock<std::mutex> lock(m_state_mutex);
    add_completion_callback(lock, std::move(callback), _impl::SyncProgressNotifier::NotifierType::download);
}

uint64_t SyncSession::register_progress_notifier(std::function<SyncProgressNotifierCallback> notifier,
                                                 NotifierType direction, bool is_streaming)
{
    return m_progress_notifier.register_callback(std::move(notifier), direction, is_streaming);
}

void SyncSession::unregister_progress_notifier(uint64_t token)
{
    m_progress_notifier.unregister_callback(token);
}

uint64_t SyncSession::register_connection_change_callback(std::function<ConnectionStateCallback> callback)
{
    return m_connection_change_notifier.add_callback(callback);
}

void SyncSession::unregister_connection_change_callback(uint64_t token)
{
    m_connection_change_notifier.remove_callback(token);
}

void SyncSession::set_multiplex_identifier(std::string multiplex_identity)
{
    m_multiplex_identity = std::move(multiplex_identity);
}

SyncSession::PublicState SyncSession::get_public_state() const
{
    if (m_state == nullptr) {
        return PublicState::Inactive;
    }
    else if (m_state == &State::active) {
        return PublicState::Active;
    }
    else if (m_state == &State::dying) {
        return PublicState::Dying;
    }
    else if (m_state == &State::inactive) {
        return PublicState::Inactive;
    }
    else if (m_state == &State::waiting_for_access_token) {
        return PublicState::WaitingForAccessToken;
    }
    REALM_UNREACHABLE();
}

SyncSession::~SyncSession() {}

SyncSession::PublicState SyncSession::state() const
{
    std::unique_lock<std::mutex> lock(m_state_mutex);
    return get_public_state();
}

SyncSession::ConnectionState SyncSession::connection_state() const
{
    std::unique_lock<std::mutex> lock(m_state_mutex);
    return m_connection_state;
}

void SyncSession::update_configuration(SyncConfig new_config)
{
    while (true) {
        std::unique_lock<std::mutex> lock(m_state_mutex);
        if (m_state != &State::inactive) {
            // Changing the state releases the lock, which means that by the
            // time we reacquire the lock the state may have changed again
            // (either due to one of the callbacks being invoked or another
            // thread coincidentally doing something). We just attempt to keep
            // switching it to inactive until it stays there.
            advance_state(lock, State::inactive);
            continue;
        }

        REALM_ASSERT(m_state == &State::inactive);
        REALM_ASSERT(!m_session);
        REALM_ASSERT(m_config.user == new_config.user);
        m_config = std::move(new_config);
        break;
    }
    revive_if_needed();
}

// Represents a reference to the SyncSession from outside of the sync subsystem.
// We attempt to keep the SyncSession in an active state as long as it has an external reference.
class SyncSession::ExternalReference {
public:
    ExternalReference(std::shared_ptr<SyncSession> session)
        : m_session(std::move(session))
    {
    }

    ~ExternalReference()
    {
        m_session->did_drop_external_reference();
    }

private:
    std::shared_ptr<SyncSession> m_session;
};

std::shared_ptr<SyncSession> SyncSession::external_reference()
{
    std::unique_lock<std::mutex> lock(m_state_mutex);

    if (auto external_reference = m_external_reference.lock())
        return std::shared_ptr<SyncSession>(external_reference, this);

    auto external_reference = std::make_shared<ExternalReference>(shared_from_this());
    m_external_reference = external_reference;
    return std::shared_ptr<SyncSession>(external_reference, this);
}

std::shared_ptr<SyncSession> SyncSession::existing_external_reference()
{
    std::unique_lock<std::mutex> lock(m_state_mutex);

    if (auto external_reference = m_external_reference.lock())
        return std::shared_ptr<SyncSession>(external_reference, this);

    return nullptr;
}

void SyncSession::did_drop_external_reference()
{
    std::unique_lock<std::mutex> lock(m_state_mutex);

    // If the session is being resurrected we should not close the session.
    if (!m_external_reference.expired())
        return;

    m_state->close(lock, *this);
}

uint64_t SyncProgressNotifier::register_callback(std::function<SyncProgressNotifierCallback> notifier,
                                                 NotifierType direction, bool is_streaming)
{
    std::function<void()> invocation;
    uint64_t token_value = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        token_value = m_progress_notifier_token++;
        NotifierPackage package{std::move(notifier), util::none, m_local_transaction_version, is_streaming,
                                direction == NotifierType::download};
        if (!m_current_progress) {
            // Simply register the package, since we have no data yet.
            m_packages.emplace(token_value, std::move(package));
            return token_value;
        }
        bool skip_registration = false;
        invocation = package.create_invocation(*m_current_progress, skip_registration);
        if (skip_registration) {
            token_value = 0;
        }
        else {
            m_packages.emplace(token_value, std::move(package));
        }
    }
    invocation();
    return token_value;
}

void SyncProgressNotifier::unregister_callback(uint64_t token)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_packages.erase(token);
}

void SyncProgressNotifier::update(uint64_t downloaded, uint64_t downloadable, uint64_t uploaded, uint64_t uploadable,
                                  uint64_t download_version, uint64_t snapshot_version)
{
    // Ignore progress messages from before we first receive a DOWNLOAD message
    if (download_version == 0)
        return;

    std::vector<std::function<void()>> invocations;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_current_progress = Progress{uploadable, downloadable, uploaded, downloaded, snapshot_version};

        for (auto it = m_packages.begin(); it != m_packages.end();) {
            bool should_delete = false;
            invocations.emplace_back(it->second.create_invocation(*m_current_progress, should_delete));
            it = should_delete ? m_packages.erase(it) : std::next(it);
        }
    }
    // Run the notifiers only after we've released the lock.
    for (auto& invocation : invocations)
        invocation();
}

void SyncProgressNotifier::set_local_version(uint64_t snapshot_version)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_local_transaction_version = snapshot_version;
}

std::function<void()> SyncProgressNotifier::NotifierPackage::create_invocation(Progress const& current_progress,
                                                                               bool& is_expired)
{
    uint64_t transferrable;
    if (is_streaming) {
        transferrable = is_download ? current_progress.downloadable : current_progress.uploadable;
    }
    else if (captured_transferrable) {
        transferrable = *captured_transferrable;
    }
    else {
        if (is_download)
            captured_transferrable = current_progress.downloadable;
        else {
            // If the sync client has not yet processed all of the local
            // transactions then the uploadable data is incorrect and we should
            // not invoke the callback
            if (snapshot_version > current_progress.snapshot_version)
                return [] {};
            captured_transferrable = current_progress.uploadable;
        }
        transferrable = *captured_transferrable;
    }

    uint64_t transferred = is_download ? current_progress.downloaded : current_progress.uploaded;
    // A notifier is expired if at least as many bytes have been transferred
    // as were originally considered transferrable.
    is_expired = !is_streaming && transferred >= transferrable;
    return [=, notifier = notifier] {
        notifier(transferred, transferrable);
    };
}

uint64_t SyncSession::ConnectionChangeNotifier::add_callback(std::function<ConnectionStateCallback> callback)
{
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    auto token = m_next_token++;
    m_callbacks.push_back({std::move(callback), token});
    return token;
}

void SyncSession::ConnectionChangeNotifier::remove_callback(uint64_t token)
{
    Callback old;
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        auto it = find_if(begin(m_callbacks), end(m_callbacks), [=](const auto& c) {
            return c.token == token;
        });
        if (it == end(m_callbacks)) {
            return;
        }

        size_t idx = distance(begin(m_callbacks), it);
        if (m_callback_index != npos) {
            if (m_callback_index >= idx)
                --m_callback_index;
        }
        --m_callback_count;

        old = std::move(*it);
        m_callbacks.erase(it);
    }
}

void SyncSession::ConnectionChangeNotifier::invoke_callbacks(ConnectionState old_state, ConnectionState new_state)
{
    std::unique_lock<std::mutex> lock(m_callback_mutex);
    m_callback_count = m_callbacks.size();
    for (++m_callback_index; m_callback_index < m_callback_count; ++m_callback_index) {
        // acquire a local reference to the callback so that removing the
        // callback from within it can't result in a dangling pointer
        auto cb = m_callbacks[m_callback_index].fn;
        lock.unlock();
        cb(old_state, new_state);
        lock.lock();
    }
    m_callback_index = npos;
}
