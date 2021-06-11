
#ifndef REALM_NOINST_CLIENT_RESET_OPERATION_HPP
#define REALM_NOINST_CLIENT_RESET_OPERATION_HPP

#include <realm/binary_data.hpp>
#include <realm/util/logger.hpp>
#include <realm/util/optional.hpp>
#include <realm/sync/protocol.hpp>
#include <realm/util/aes_cryptor.hpp>

namespace realm {
namespace _impl {

// A ClientResetOperation object is used per client session to keep track of
// state Realm download.
class ClientResetOperation {
public:
    util::Logger& logger;

    ClientResetOperation(util::Logger& logger, const std::string& realm_path, bool seamless_loss,
                         util::Optional<std::array<char, 64>> encryption_key);

    // When the client has received the salted file ident from the server, it
    // should deliver the ident to the ClientResetOperation object. The ident
    // will be inserted in the Realm after download.
    bool finalize(sync::SaltedFileIdent salted_file_ident);

    realm::VersionID get_client_reset_old_version();
    realm::VersionID get_client_reset_new_version();

private:
    const std::string m_realm_path;
    bool m_seamless_loss;
    util::Optional<std::array<char, 64>> m_encryption_key;
#if REALM_ENABLE_ENCRYPTION
    std::unique_ptr<util::AESCryptor> m_aes_cryptor;
#endif

    sync::SaltedFileIdent m_salted_file_ident = {0, 0};
    realm::VersionID m_client_reset_old_version;
    realm::VersionID m_client_reset_new_version;
};

// Implementation

inline realm::VersionID ClientResetOperation::get_client_reset_old_version()
{
    return m_client_reset_old_version;
}

inline realm::VersionID ClientResetOperation::get_client_reset_new_version()
{
    return m_client_reset_new_version;
}

} // namespace _impl
} // namespace realm

#endif // REALM_NOINST_CLIENT_RESET_OPERATION_HPP
