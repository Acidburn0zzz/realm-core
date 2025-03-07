#include <realm.hpp>
#include <iostream>
#include <realm/history.hpp>

const char* legend =
    "Simple tool to output the JSON representation of a Realm:\n"
    "  realm2json [--link-depth N] [--output-mode N] <.realm file>\n"
    "\n"
    "Options:\n"
    " --schema: Just output the schema of the realm\n"
    " --link-depth: How deep to traverse linking objects (use -1 for infinite). See test_json.cpp "
    "for more details. Defaults to 0.\n"
    " --output-mode: Optional formatting for the output \n"
    "      0 - JSON Object\n"
    "      1 - MongoDB Extended JSON (XJSON)\n"
    "      2 - An extension of XJSON that adds wrappers for embdded objects, links, dictionaries, etc\n"
    "\n";

template <typename FormatStr>
void abort_if(bool cond, FormatStr fmt)
{
    if (!cond) {
        return;
    }

    fputs(fmt, stderr);
    std::exit(1);
}

template <typename FormatStr, typename... Args>
void abort_if(bool cond, FormatStr fmt, Args... args)
{
    if (!cond) {
        return;
    }

    fprintf(stderr, fmt, args...);
    std::exit(1);
}

int main(int argc, char const* argv[])
{
    std::map<std::string, std::string> renames;
    size_t link_depth = 0;
    bool output_schema = false;
    realm::JSONOutputMode output_mode = realm::output_mode_json;

    abort_if(argc <= 1, legend);

    // Parse from 1'st argument until before source args
    for (int idx = 1; idx < argc - 1; ++idx) {
        realm::StringData arg(argv[idx]);
        if (arg == "--schema") {
            output_schema = true;
        }
        else if (arg == "--link-depth") {
            link_depth = strtol(argv[++idx], nullptr, 0);
        }
        else if (arg == "--output-mode") {
            auto output_mode_val = strtol(argv[++idx], nullptr, 0);
            abort_if(output_mode_val > 2, "Received unknown value for output_mode option: %d", output_mode_val);

            switch (output_mode_val) {
                case 0: {
                    output_mode = realm::output_mode_json;
                    break;
                }
                case 1: {
                    output_mode = realm::output_mode_xjson;
                    break;
                }
                case 2: {
                    output_mode = realm::output_mode_xjson_plus;
                    break;
                }
            }
        }
        else {
            abort_if(true, "Received unknown option '%s' - please see description below\n\n%s", argv[idx], legend);
        }
    }

    std::string path = argv[argc - 1];

    try {
        // First we try to open in read_only mode. In this way we can also open
        // realms with a client history
        realm::Group g(path);
        if (output_schema) {
            g.schema_to_json(std::cout, &renames);
        }
        else {
            g.to_json(std::cout, link_depth, &renames, output_mode);
        }
    }
    catch (const realm::FileFormatUpgradeRequired&) {
        // In realm history
        // Last chance - this one must succeed
        auto hist = realm::make_in_realm_history(path);
        realm::DBOptions options;
        options.allow_file_format_upgrade = true;

        auto db = realm::DB::create(*hist, options);

        std::cerr << "File upgraded to latest version: " << path << std::endl;

        auto tr = db->start_read();
        tr->to_json(std::cout, link_depth, &renames, output_mode);
    }

    return 0;
}
