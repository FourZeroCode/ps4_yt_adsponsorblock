#include "util.h"
#include <cctype>
#include "patch.h"
#include "version.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>
#include <orbis/libkernel.h>


extern "C" void mh_log(const char* fmt, ...);

#define MH_LOG(fmt, ...) mh_log("[minihook] " fmt "\n", ##__VA_ARGS__)

namespace {

    bool looks_printable_text(const uint8_t* data, size_t len, double min_ratio) {
        if (!data || len == 0) return false;
        size_t printable = 0;
        for (size_t i = 0; i < len; ++i) {
            uint8_t c = data[i];
            if ((c >= 0x20 && c < 0x7F) || c == '\t' || c == '\r' || c == '\n') {
                ++printable;
            }
        }
        return static_cast<double>(printable) / static_cast<double>(len) >= min_ratio;
    }

    std::vector<uint8_t> read_remote_buffer(uint64_t addr, size_t length) {
        std::vector<uint8_t> out(length, 0);
        if (!addr || length == 0 || !is_canonical_address(addr)) {
            return out;
        }
        sys_proc_ro(addr, out.data(), out.size());
        return out;
    }

    bool read_remote_ascii(uint64_t addr, size_t max_bytes, std::string& out) {
        if (!addr || !is_canonical_address(addr) || max_bytes == 0) {
            return false;
        }
        size_t fetch = std::min<size_t>(max_bytes, 2048);
        std::vector<char> buffer(fetch + 1, 0);
        sys_proc_ro(addr, buffer.data(), fetch);
        buffer[fetch] = '\0';

        size_t actual = strnlen(buffer.data(), fetch);
        if (!looks_printable_text(reinterpret_cast<const uint8_t*>(buffer.data()), actual, 0.55)) {
            return false;
        }
        out.assign(buffer.data(), actual);
        return true;
    }


    // Create the user's initial config only when no config.json exists.
    // Existing files are never opened for writing or modified.
    bool ensure_default_config_file() {
        const char* path = "/data/youtube/config.json";
        int existing_fd = sceKernelOpen(path, 0, 0);
        if (existing_fd >= 0) {
            sceKernelClose(existing_fd);
            return true;
        }

        static const char kDefaultConfig[] = R"json({
  "categories": [
    "sponsor",
    "intro",
    "outro",
    "selfpromo",
    "interaction",
    "music_offtopic",
    "preview",
    "filler",
    "exclusive_access",
    "poi_highlight",
    "chapter",
    "hook"
  ],
  "skipMode": "confirm",
  "checkUpdatesOnStartup": true,
  "adBlockEnabled": true,
  "sponsorBlockEnabled": true
}
)json";

        int fd = sceKernelOpen(path, 0x601, 0666); // O_WRONLY | O_CREAT | O_TRUNC
        if (fd < 0) {
            MH_LOG("Could not create default config.json (%d)", fd);
            return false;
        }

        const size_t length = sizeof(kDefaultConfig) - 1;
        const size_t written = sceKernelWrite(fd, kDefaultConfig, length);
        sceKernelClose(fd);
        if (written != length) {
            MH_LOG("Default config.json write incomplete (%zu/%zu)", written, length);
            return false;
        }

        MH_LOG("Created default config.json (%zu bytes)", length);
        return true;
    }

    // Default SponsorBlock categories used when config.json is missing, unreadable,
    // empty, or contains no recognized category IDs. Leave empty to disable all
    // Default hard-coded categories are not used anymore, but the code is left here for future reference.
    const std::vector<std::string> kDefaultCategories = {
        // "sponsor", "intro", "outro", "music_offtopic"
    };
    std::vector<std::string> load_categories(std::string* out_skip_mode, bool* out_valid = nullptr) {
        ensure_default_config_file();
        if (out_skip_mode) *out_skip_mode = "auto";
        if (out_valid) *out_valid = false;
        const char* path = "/data/youtube/config.json";
        int fd = sceKernelOpen(path, 0, 0);
        if (fd < 0) {
            MH_LOG("config.json open failed (%d); using defaults", fd);
            return kDefaultCategories;
        }

        // config.json skipMode and categories, read until EOF.
        std::string content;
        content.reserve(1024);
        char chunk[512];
        bool read_error = false;
        for (;;) {
            size_t n = sceKernelRead(fd, chunk, sizeof(chunk));
            if (n == 0) break; // EOF
            if (n > sizeof(chunk)) {
                read_error = true;
                MH_LOG("config.json read returned invalid size %zu", n);
                break;
            }
            if (content.size() + n > 16384) {
                read_error = true;
                MH_LOG("config.json exceeds 16384 bytes");
                break;
            }
            content.append(chunk, n);
        }
        sceKernelClose(fd);

        if (read_error || content.empty()) {
            MH_LOG("config.json read failed/empty (%zu bytes); using defaults", content.size());
            return kDefaultCategories;
        }
        MH_LOG("config.json read %zu bytes", content.size());

        // Require a recognizable categories array before accepting a live reload.
        const size_t categories_key = content.find("\"categories\"");
        if (categories_key == std::string::npos) {
            MH_LOG("config.json has no categories key; retaining active settings");
            return kDefaultCategories;
        }
        const size_t categories_open = content.find('[', categories_key);
        const size_t categories_close = categories_open == std::string::npos ? std::string::npos : content.find(']', categories_open + 1);
        if (categories_open == std::string::npos || categories_close == std::string::npos) {
            MH_LOG("config.json categories array is incomplete; retaining active settings");
            return kDefaultCategories;
        }
        const std::string categories_content = content.substr(categories_open + 1, categories_close - categories_open - 1);
        if (out_valid) *out_valid = true;

        // The PS4 config is read as raw bytes until EOF;
        if (content.size() >= 3 &&
            static_cast<unsigned char>(content[0]) == 0xEF &&
            static_cast<unsigned char>(content[1]) == 0xBB &&
            static_cast<unsigned char>(content[2]) == 0xBF) {
            content.erase(0, 3);
        }

        // Read skipMode from the config file. Unknown/missing values
        // deliberately default to automatic skipping for backwards compatibility.
        if (out_skip_mode) {
            const size_t key_pos = content.find("\"skipMode\"");
            if (key_pos != std::string::npos) {
                const size_t colon = content.find(':', key_pos + 10);
                if (colon != std::string::npos) {
                    const size_t value_quote = content.find('"', colon + 1);
                    if (value_quote != std::string::npos) {
                        const size_t value_end = content.find('"', value_quote + 1);
                        if (value_end != std::string::npos) {
                            const std::string value = content.substr(value_quote + 1, value_end - value_quote - 1);
                            if (value == "confirm" || value == "silent") *out_skip_mode = value;
                            else *out_skip_mode = "auto";
                        }
                    }
                }
            }
            MH_LOG("SponsorBlock skipMode: %s", out_skip_mode->c_str());
        }

        // Match only supported SponsorBlock category IDs. Iterating this unique list
        // also prevents duplicate entries in the resulting JavaScript array.
        const char* known[] = {
          "sponsor", "intro", "outro", "selfpromo", "interaction",
          "music_offtopic", "preview", "filler", "exclusive_access",
          "poi_highlight", "chapter", "hook"
        };

        std::vector<std::string> categories;
        for (const char* candidate : known) {
            const std::string quoted_id = std::string("\"") + candidate + "\"";
            if (categories_content.find(quoted_id) != std::string::npos) {
                categories.push_back(candidate);
            }
        }

        if (categories.empty()) {
            MH_LOG("config.json contains no enabled recognized SponsorBlock categories");
            return categories;
        }

        MH_LOG("Found %zu recognized category IDs in config.json", categories.size());
        return categories;
    }

    std::string categories_to_js_array(const std::vector<std::string>& categories) {
        std::string result = "[";
        for (size_t i = 0; i < categories.size(); ++i) {
            if (i != 0) result += ", ";
            // Category parser only permits lowercase letters, digits, and underscores.
            result += "'" + categories[i] + "'";
        }
        result += "]";
        return result;
    }

    std::string g_js_payload;
    bool g_js_payload_loaded = false;

}  // namespace

bool get_sponsorblock_config_json(char* out, size_t out_size) {
    if (!out || out_size == 0) return false;
    std::string skip_mode = "auto";
    bool valid = false;
    const std::vector<std::string> categories = load_categories(&skip_mode, &valid);
    if (!valid) return false;

    // Emit standard JSON strings for the JavaScript configuration bridge.
    std::string json = "{\"categories\":[";
    for (size_t i = 0; i < categories.size(); ++i) {
        if (i) json += ",";
        json += "\"" + categories[i] + "\"";
    }
    bool check_updates_on_startup = true;
    bool ad_block_enabled = true;
    bool sponsor_block_enabled = true;
    const char* config_path = "/data/youtube/config.json";
    int config_fd = sceKernelOpen(config_path, 0, 0);
    if (config_fd >= 0) {
        std::string config_content;
        char config_chunk[512];
        for (;;) {
            size_t n = sceKernelRead(config_fd, config_chunk, sizeof(config_chunk));
            if (n == 0 || n > sizeof(config_chunk) || config_content.size() + n > 16384) break;
            config_content.append(config_chunk, n);
        }
        sceKernelClose(config_fd);
        const size_t setting_key = config_content.find("\"checkUpdatesOnStartup\"");
        if (setting_key != std::string::npos) {
            const size_t colon = config_content.find(':', setting_key);
            if (colon != std::string::npos) {
                const size_t value = config_content.find_first_not_of(" \t\r\n", colon + 1);
                if (value != std::string::npos && config_content.compare(value, 5, "false") == 0)
                    check_updates_on_startup = false;
            }
        }
        const size_t ad_block_key = config_content.find("\"adBlockEnabled\"");
        if (ad_block_key != std::string::npos) {
            const size_t colon = config_content.find(':', ad_block_key);
            if (colon != std::string::npos) {
                const size_t value = config_content.find_first_not_of(" \t\r\n", colon + 1);
                if (value != std::string::npos && config_content.compare(value, 5, "false") == 0)
                    ad_block_enabled = false;
            }
        }
        const size_t sponsor_block_key = config_content.find("\"sponsorBlockEnabled\"");
        if (sponsor_block_key != std::string::npos) {
            const size_t colon = config_content.find(':', sponsor_block_key);
            if (colon != std::string::npos) {
                const size_t value = config_content.find_first_not_of(" \t\r\n", colon + 1);
                if (value != std::string::npos && config_content.compare(value, 5, "false") == 0)
                    sponsor_block_enabled = false;
            }
        }
    }
    json += "],\"skipMode\":\"" + skip_mode + "\",\"checkUpdatesOnStartup\":" + (check_updates_on_startup ? "true" : "false") + ",\"adBlockEnabled\":" + (ad_block_enabled ? "true" : "false") + ",\"sponsorBlockEnabled\":" + (sponsor_block_enabled ? "true" : "false") + "}";
    if (json.size() + 1 > out_size) return false;
    memcpy(out, json.c_str(), json.size() + 1);
    return true;
}

bool is_update_check_on_startup_enabled(void) {
    const char* path = "/data/youtube/config.json";
    int fd = sceKernelOpen(path, 0, 0);
    if (fd < 0) return true; // Enabled by default for existing installations.
    std::string content;
    char chunk[512];
    for (;;) {
        size_t n = sceKernelRead(fd, chunk, sizeof(chunk));
        if (n == 0 || n > sizeof(chunk) || content.size() + n > 16384) break;
        content.append(chunk, n);
    }
    sceKernelClose(fd);
    const size_t key = content.find("\"checkUpdatesOnStartup\"");
    if (key == std::string::npos) return true;
    const size_t colon = content.find(':', key);
    if (colon == std::string::npos) return true;
    const size_t value = content.find_first_not_of(" \t\r\n", colon + 1);
    if (value == std::string::npos) return true;
    return content.compare(value, 5, "false") != 0;
}

bool save_sponsorblock_config_json(const char* json, size_t length) {
    if (!json || length == 0 || length > 2048) return false;
    std::string content(json, length);
    if (content.find("\"categories\"") == std::string::npos) return false;
    if (content.find("\"skipMode\":\"auto\"") == std::string::npos &&
        content.find("\"skipMode\":\"confirm\"") == std::string::npos &&
        content.find("\"skipMode\":\"silent\"") == std::string::npos) return false;
    const char* known[] = {"sponsor", "intro", "outro", "selfpromo", "interaction", "music_offtopic", "preview", "filler", "exclusive_access", "poi_highlight", "chapter", "hook"};
    // Validate that every category token in the array belongs to the supported set.
    size_t key = content.find("\"categories\"");
    size_t open = content.find('[', key), close = content.find(']', open == std::string::npos ? key : open + 1);
    if (open == std::string::npos || close == std::string::npos) return false;
    std::string arr = content.substr(open + 1, close - open - 1);
    std::vector<std::string> categories;
    for (size_t i = 0; i < arr.size();) {
        while (i < arr.size() && (std::isspace(static_cast<unsigned char>(arr[i])) || arr[i] == ',')) ++i;
        if (i == arr.size()) break;
        if (arr[i] != '"') return false;
        size_t end = arr.find('"', i + 1);
        if (end == std::string::npos) return false;
        std::string id = arr.substr(i + 1, end - i - 1);
        bool valid = false;
        for (const char* candidate : known) if (id == candidate) valid = true;
        if (!valid) return false;
        categories.push_back(id);
        i = end + 1;
        while (i < arr.size() && std::isspace(static_cast<unsigned char>(arr[i]))) ++i;
        if (i < arr.size() && arr[i] != ',') return false;
    }

    // Keep config.json human-readable: write one category per line.
    std::string skip_mode = content.find("\"skipMode\":\"silent\"") != std::string::npos ? "silent" :
                            content.find("\"skipMode\":\"confirm\"") != std::string::npos ? "confirm" : "auto";
    std::string formatted = "{\n  \"categories\": [";
    if (!categories.empty()) formatted += "\n";
    for (size_t i = 0; i < categories.size(); ++i) {
        formatted += "    \"" + categories[i] + "\"";
        if (i + 1 < categories.size()) formatted += ",";
        formatted += "\n";
    }
    bool check_updates_on_startup = true;
    bool ad_block_enabled = true;
    bool sponsor_block_enabled = true;
    auto read_boolean_setting = [](const std::string& source, const char* name, bool default_value) {
        const std::string key = std::string("\"") + name + "\"";
        const size_t setting_key = source.find(key);
        if (setting_key == std::string::npos) return default_value;
        const size_t colon = source.find(':', setting_key + key.size());
        if (colon == std::string::npos) return default_value;
        const size_t value = source.find_first_not_of(" \t\r\n", colon + 1);
        if (value == std::string::npos) return default_value;
        return source.compare(value, 5, "false") != 0;
    };
    std::string existing_content;
    if (content.find("\"checkUpdatesOnStartup\"") == std::string::npos ||
        content.find("\"adBlockEnabled\"") == std::string::npos ||
        content.find("\"sponsorBlockEnabled\"") == std::string::npos) {
        const char* existing_path = "/data/youtube/config.json";
        int existing_fd = sceKernelOpen(existing_path, 0, 0);
        if (existing_fd >= 0) {
            char existing_chunk[512];
            for (;;) {
                size_t n = sceKernelRead(existing_fd, existing_chunk, sizeof(existing_chunk));
                if (n == 0 || n > sizeof(existing_chunk) || existing_content.size() + n > 16384) break;
                existing_content.append(existing_chunk, n);
            }
            sceKernelClose(existing_fd);
        }
    }
    check_updates_on_startup = read_boolean_setting(
        content, "checkUpdatesOnStartup",
        read_boolean_setting(existing_content, "checkUpdatesOnStartup", true));
    ad_block_enabled = read_boolean_setting(
        content, "adBlockEnabled",
        read_boolean_setting(existing_content, "adBlockEnabled", true));
    sponsor_block_enabled = read_boolean_setting(
        content, "sponsorBlockEnabled",
        read_boolean_setting(existing_content, "sponsorBlockEnabled", true));
    formatted += "  ],\n  \"skipMode\": \"" + skip_mode +
                 "\",\n  \"checkUpdatesOnStartup\": " + (check_updates_on_startup ? "true" : "false") +
                 ",\n  \"adBlockEnabled\": " + (ad_block_enabled ? "true" : "false") +
                 ",\n  \"sponsorBlockEnabled\": " + (sponsor_block_enabled ? "true" : "false") + "\n}\n";

    const char* path = "/data/youtube/config.json";
    int fd = sceKernelOpen(path, 0x601, 0666); // O_WRONLY | O_CREAT | O_TRUNC
    if (fd < 0) return false;
    size_t written = sceKernelWrite(fd, formatted.data(), formatted.size());
    sceKernelClose(fd);
    if (written != formatted.size()) return false;
    MH_LOG("Saved SponsorBlock config.json (%zu bytes)", formatted.size());
    return true;
}

bool is_canonical_address(uint64_t addr) {
    return (addr & 0xFFFF800000000000ull) == 0;
}

void log_preview_bounded(const uint8_t* b, size_t n, size_t max_chars) {
    size_t take = n < max_chars ? n : max_chars;
    char buf[256 + 1];
    if (take > 256) take = 256;
    for (size_t i = 0; i < take; ++i) {
        uint8_t c = b[i];
        buf[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    buf[take] = 0;
    MH_LOG("[execute.preview 0..%zu] %s", take, buf);
}

bool ensure_js_payload_loaded() {
    if (g_js_payload_loaded) {
        return true;
    }

    // Use the embedded JavaScript payload.
    static const char kHardcodedJs[] =
        "(function(){\n"
        "  if (window.__MH_SPONSORBLOCK_LOADED) { return; }\n"
        "  window.__MH_SPONSORBLOCK_LOADED = true;\n"
        "\n"
        "  function showToast(title, subtitle) {\n"
        "    try {\n"
        "      var popupAction = {\n"
        "        openPopupAction: {\n"
        "          popupType: 'TOAST',\n"
        "          popup: {\n"
        "            overlayToastRenderer: {\n"
        "              title: { simpleText: title },\n"
        "              subtitle: { simpleText: subtitle }\n"
        "            }\n"
        "          }\n"
        "        }\n"
        "      };\n"
        "      for (var key in window._yttv) {\n"
        "        if (window._yttv[key] && window._yttv[key].instance && window._yttv[key].instance.resolveCommand) {\n"
        "          window._yttv[key].instance.resolveCommand(popupAction);\n"
        "          break;\n"
        "        }\n"
        "      }\n"
        "    } catch(e) {}\n"
        "  }\n"
        "\n"
        "  var updatePromptShown = false;\n"
        "  var activeUpdateKeyHandler = null;\n"
        "  var activeUpdateKeyUpHandler = null;\n"
        "  var activeUpdateKeyPressHandler = null;\n"
        "  var updateInputGuardUntil = 0;\n"
        "  function suppressUpdateInputEvent(event) { if (event.preventDefault) event.preventDefault(); if (event.stopPropagation) event.stopPropagation(); if (event.stopImmediatePropagation) event.stopImmediatePropagation(); if (event.returnValue !== undefined) event.returnValue = false; event.cancelBubble = true; }\n"
        "  window.addEventListener('keydown', function(event) {\n"
        "    if (activeUpdateKeyHandler) activeUpdateKeyHandler(event); else if (Date.now() < updateInputGuardUntil) suppressUpdateInputEvent(event);\n"
        "  }, true);\n"
        "  window.addEventListener('keyup', function(event) {\n"
        "    if (activeUpdateKeyUpHandler) activeUpdateKeyUpHandler(event); else if (Date.now() < updateInputGuardUntil) suppressUpdateInputEvent(event);\n"
        "  }, true);\n"
        "  window.addEventListener('keypress', function(event) {\n"
        "    if (activeUpdateKeyPressHandler) activeUpdateKeyPressHandler(event); else if (Date.now() < updateInputGuardUntil) suppressUpdateInputEvent(event);\n"
        "  }, true);\n"
        "  function showUpdatePrompt(version) {\n"
        "    var overlay = document.createElement('div');\n"
        "    overlay.style.cssText = 'position:fixed;top:72px;right:72px;z-index:2147483647;display:block;pointer-events:auto;font-family:inherit;';\n"
        "    var panel = document.createElement('div'); panel.tabIndex = -1;\n"
        "    panel.style.cssText = 'pointer-events:auto;display:inline-block;vertical-align:top;box-sizing:border-box;width:auto;max-width:calc(100vw - 144px);padding:26px 24px;border:0;border-radius:17px;background:rgba(24,24,24,1);color:#fff;text-align:left;box-shadow:none;';\n"
        "    var title = document.createElement('div'); title.textContent = 'AdlessTube'; title.style.cssText = 'font-family:inherit;font-size:30px;font-weight:500;color:#f1f1f1;margin-bottom:14px;text-align:left;position:relative;top:1px;';\n"
        "    var detail = document.createElement('div'); detail.textContent = 'Version ' + version + ' is available'; detail.style.cssText = 'font-family:inherit;font-size:24px;font-weight:400;color:#aaa;text-align:left;margin-bottom:12px;';\n"
        "    var buttons = document.createElement('div'); buttons.style.cssText = 'display:flex;justify-content:center;';\n"
        "    var later = document.createElement('button'); later.textContent = 'Later'; later.style.cssText = '-webkit-appearance:none;appearance:none;font-size:20px;font-weight:bold;padding:12px 18px;border:0;border-radius:12px;outline:none;box-shadow:none;background:#333;color:#fff;';\n"
        "    var buttonSpacer = document.createElement('div'); buttonSpacer.style.cssText = 'width:10px;min-width:10px;height:1px;flex:0 0 10px;';\n"
        "    var update = document.createElement('button'); update.textContent = 'Update'; update.style.cssText = '-webkit-appearance:none;appearance:none;font-size:20px;font-weight:bold;padding:12px 18px;border:0;border-radius:12px;outline:none;box-shadow:none;background:#fff;color:#111;';\n"
        "    var selected = 1;\n"
        "    var pendingActivate = false;\n"
        "    var directionHeld = {};\n"
        "    var suspendedBackground = [];\n"
        "    function suspendBackground() {\n"
        "      try {\n"
        "        var active = document.activeElement;\n"
        "        if (active && active !== document.body && active.blur) active.blur();\n"
        "        var children = document.body ? document.body.children : [];\n"
        "        for (var n = 0; n < children.length; n++) {\n"
        "          var node = children[n];\n"
        "          if (!node || node === overlay) continue;\n"
        "          suspendedBackground.push({node:node, inert:!!node.inert, ariaHidden:node.getAttribute('aria-hidden'), pointerEvents:node.style.pointerEvents});\n"
        "          node.inert = true;\n"
        "          node.setAttribute('aria-hidden', 'true');\n"
        "          node.style.pointerEvents = 'none';\n"
        "        }\n"
        "      } catch(e) {}\n"
        "    }\n"
        "    function restoreBackground() {\n"
        "      for (var n = 0; n < suspendedBackground.length; n++) {\n"
        "        var saved = suspendedBackground[n]; var node = saved.node;\n"
        "        if (!node) continue;\n"
        "        try { node.inert = saved.inert; if (saved.ariaHidden === null) node.removeAttribute('aria-hidden'); else node.setAttribute('aria-hidden', saved.ariaHidden); node.style.pointerEvents = saved.pointerEvents; } catch(e) {}\n"
        "      }\n"
        "      suspendedBackground = [];\n"
        "    }\n"
        "    function closePrompt() { updateInputGuardUntil = Date.now() + 650; activeUpdateKeyHandler = null; activeUpdateKeyUpHandler = null; activeUpdateKeyPressHandler = null; window.removeEventListener('keydown', keyHandler, true); window.removeEventListener('keyup', keyUpHandler, true); window.removeEventListener('keypress', keyPressHandler, true); window.removeEventListener('click', clickGuard, true); window.removeEventListener('pointerdown', pointerGuard, true); window.removeEventListener('pointerup', pointerGuard, true); window.removeEventListener('mousedown', pointerGuard, true); window.removeEventListener('mouseup', pointerGuard, true); window.removeEventListener('touchstart', pointerGuard, true); window.removeEventListener('touchend', pointerGuard, true); window.removeEventListener('focusin', focusGuard, true); restoreBackground(); if (overlay.parentNode) overlay.parentNode.removeChild(overlay); }\n"
        "    function paint() { later.style.background = selected === 0 ? '#fff' : '#333'; later.style.color = selected === 0 ? '#111' : '#fff'; update.style.background = selected === 1 ? '#fff' : '#333'; update.style.color = selected === 1 ? '#111' : '#fff'; }\n"
        "    later.onclick = function(event) { if (event) { event.preventDefault(); event.stopPropagation(); } closePrompt(); showToast('Update skipped', 'You can update later'); };\n"
        "    update.onclick = function(event) { if (event) { event.preventDefault(); event.stopPropagation(); } closePrompt(); updateInstallStarted=true; lastUpdateToastState='downloading'; showToast('Downloading update', 'Please wait...'); var r=new XMLHttpRequest(); r.timeout=5000; r.open('GET','http://127.0.0.1:4040/__start_update',true); r.onload=function(){ if(r.status!==200) showToast('Update failed','Could not start installation.'); else pollUpdateStatus(); }; r.onerror=function(){ showToast('Update failed','Could not contact the local updater.'); }; r.send(); };\n"
        "    var keyUpHandler = function(event) {\n"
        "      var key = event.key || '';\n"
        "      var code = event.keyCode || event.which || 0;\n"
        "      if (code === 37 || code === 32782 || code === 32787) directionHeld[37] = false;\n"
        "      if (code === 39 || code === 32783 || code === 32788) directionHeld[39] = false;\n"
        "      var handled = key === 'ArrowLeft' || key === 'ArrowRight' || key === 'Enter' || key === ' ' || key === 'Spacebar' || key === 'x' || key === 'X' || key === 'Escape' || key === 'Backspace' || key === 'o' || key === 'O' || code === 37 || code === 39 || code === 13 || code === 32 || code === 88 || code === 27 || code === 8 || code === 79 || code === 32768 || code === 32769 || code === 32782 || code === 32783 || code === 32787 || code === 32788;\n"
        "      if (handled) suppressUpdateInputEvent(event);\n"
        "      if (overlay.parentNode && pendingActivate && (code === 32768 || code === 13 || code === 32 || key === 'Enter' || key === 'x' || key === 'X')) {\n"
        "        pendingActivate = false; updateInputGuardUntil = Date.now() + 650;\n"
        "        if (selected === 1) update.onclick(); else later.onclick();\n"
        "      }\n"
        "    };\n"
        "    var keyPressHandler = function(event) {\n"
        "      if (!overlay.parentNode) return;\n"
        "      var key = event.key || ''; var code = event.keyCode || event.which || 0;\n"
        "      if (key === 'Enter' || key === ' ' || key === 'Spacebar' || key === 'x' || key === 'X' || code === 13 || code === 32 || code === 88 || code === 32768 || code === 32769) {\n"
        "        if (event.preventDefault) event.preventDefault(); if (event.stopPropagation) event.stopPropagation(); if (event.stopImmediatePropagation) event.stopImmediatePropagation();\n"
        "        if (event.returnValue !== undefined) event.returnValue = false; event.cancelBubble = true;\n"
        "      }\n"
        "    };\n"
        "    var clickGuard = function(event) {\n"
        "      if (!overlay.parentNode) return;\n"
        "      var target = event.target;\n"
        "      if (target && (target === panel || (panel.contains && panel.contains(target)))) return;\n"
        "      if (event.preventDefault) event.preventDefault(); if (event.stopPropagation) event.stopPropagation(); if (event.stopImmediatePropagation) event.stopImmediatePropagation();\n"
        "      if (event.returnValue !== undefined) event.returnValue = false; event.cancelBubble = true;\n"
        "    };\n"
        "    var pointerGuard = function(event) {\n"
        "      if (!overlay.parentNode) return;\n"
        "      var target = event.target;\n"
        "      if (target && (target === panel || (panel.contains && panel.contains(target)))) return;\n"
        "      if (event.preventDefault) event.preventDefault(); if (event.stopPropagation) event.stopPropagation(); if (event.stopImmediatePropagation) event.stopImmediatePropagation();\n"
        "      if (event.returnValue !== undefined) event.returnValue = false; event.cancelBubble = true;\n"
        "    };\n"
        "    var focusGuard = function(event) {\n"
        "      if (!overlay.parentNode) return;\n"
        "      var target = event.target;\n"
        "      if (target && (target === panel || (panel.contains && panel.contains(target)))) return;\n"
        "      try { (selected === 1 ? update : later).focus(); } catch(e) {}\n"
        "    };\n"
        "    var keyHandler = function(event) {\n"
        "      if (!overlay.parentNode) return;\n"
        "      var key = event.key || '';\n"
        "      var code = event.keyCode || event.which || 0;\n"
        "      if (key === 'ArrowLeft' || key === 'ArrowRight' || code === 37 || code === 39 || code === 32782 || code === 32783 || code === 32787 || code === 32788) {\n"
        "        if (event.preventDefault) event.preventDefault();\n"
        "        if (event.stopPropagation) event.stopPropagation();\n"
        "        if (event.stopImmediatePropagation) event.stopImmediatePropagation();\n"
        "        var dirCode = (key === 'ArrowLeft' || code === 37 || code === 32782 || code === 32787) ? 37 : 39;\n"
        "        if (!directionHeld[dirCode]) { selected = selected === 0 ? 1 : 0; paint(); try { (selected === 1 ? update : later).focus(); } catch(e) {} }\n"
        "        directionHeld[dirCode] = true;\n"
        "        return;\n"
        "      }\n"
        "      var isBack = key === 'Escape' || key === 'Backspace' || key === 'o' || key === 'O' || code === 27 || code === 8 || code === 79 || code === 32769;\n"
        "      if (isBack) {\n"
        "        if (event.preventDefault) event.preventDefault();\n"
        "        if (event.stopPropagation) event.stopPropagation();\n"
        "        if (event.stopImmediatePropagation) event.stopImmediatePropagation();\n"
        "        closePrompt();\n"
        "        return;\n"
        "      }\n"
        "      var activate = key === 'Enter'  || key === ' ' || key === 'Spacebar' || key === 'x' || key === 'X' || code === 13 || code === 32 || code === 88 || code === 32768;\n"
        "      if (activate) {\n"
        "        suppressUpdateInputEvent(event); pendingActivate = true; return;\n"
        "      }\n"
        "      // Keep D-pad Up/Down available to YouTube; consume other input while the prompt is open.\n"
        "      if (!(key === 'ArrowUp' || key === 'ArrowDown' || code === 38 || code === 40)) {\n"
        "        if (event.preventDefault) event.preventDefault();\n"
        "        if (event.stopPropagation) event.stopPropagation();\n"
        "        if (event.stopImmediatePropagation) event.stopImmediatePropagation();\n"
        "      }\n"
        "    };\n"
        "    overlay._mhKeyHandler = keyHandler;\n"
        "    overlay._mhKeyUpHandler = keyUpHandler;\n"
        "    activeUpdateKeyHandler = keyHandler;\n"
        "    activeUpdateKeyUpHandler = keyUpHandler;\n"
        "    activeUpdateKeyPressHandler = keyPressHandler;\n"
        "    buttons.appendChild(later); buttons.appendChild(buttonSpacer); buttons.appendChild(update); panel.appendChild(title); panel.appendChild(detail); panel.appendChild(buttons); overlay.appendChild(panel); document.body.appendChild(overlay); suspendBackground(); window.addEventListener('click', clickGuard, true); window.addEventListener('pointerdown', pointerGuard, true); window.addEventListener('pointerup', pointerGuard, true); window.addEventListener('mousedown', pointerGuard, true); window.addEventListener('mouseup', pointerGuard, true); window.addEventListener('touchstart', pointerGuard, true); window.addEventListener('touchend', pointerGuard, true); window.addEventListener('focusin', focusGuard, true); paint(); try { update.focus(); } catch(e) {}\n"
        "  }\n"
        "  var updateStatusFinished = false;\n"
        "  var manualUpdateCheckActive = false;\n"
        "  var updateInstallStarted = false;\n"
        "  var lastUpdateToastState = '';\n"
        "  function pollUpdateStatus() {\n"
        "    if (updateStatusFinished || (updatePromptShown && !updateInstallStarted)) return;\n"
        "    var ports=[4040,4041,4042,4043,4044,4045,4046,4047,4048,4049];\n"
        "    var i=0;\n"
        "    function retryLater() { if (!updateStatusFinished && (!updatePromptShown || updateInstallStarted)) setTimeout(pollUpdateStatus, 1500); }\n"
        "    function next() {\n"
        "      if (i>=ports.length) { retryLater(); return; }\n"
        "      var x=new XMLHttpRequest(); x.timeout=1200;\n"
        "      x.onload=function() {\n"
        "        if (x.status!==200) { i++; next(); return; }\n"
        "        try {\n"
        "          var st=origParse(x.responseText);\n"
        "          if (st.state==='available' && !updatePromptShown) { updatePromptShown=true; showUpdatePrompt(st.version||'new'); return; }\n"
        "          if (st.state==='current') { updateStatusFinished=true; if (manualUpdateCheckActive) { manualUpdateCheckActive=false; showToast('AdlessTube', \"Is up to date: Version " PS4_YT_ADSPONSORBLOCK_VERSION "\"); } return; }\n"
        "          if (st.state==='error') { updateStatusFinished=true; return; }\n"
        "          if (st.state==='downloading' && lastUpdateToastState!=='downloading') { lastUpdateToastState='downloading'; showToast('Downloading update', 'Please wait...'); }\n"
        "          if (st.state==='installing' && lastUpdateToastState!=='installing') { lastUpdateToastState='installing'; showToast('Installing update', 'Finishing up...'); }\n"
        "          if (st.state==='installed') { updateStatusFinished=true; lastUpdateToastState='installed'; showToast('Update installed successfully', 'Please restart YouTube to load the new version.'); return; }\n"
        "          if (st.state==='install_error') { updateStatusFinished=true; lastUpdateToastState='install_error'; showToast('Update failed', st.message || 'Your previous plugin was preserved or restored.'); return; }\n"
        "        } catch(e) {}\n"
        "        retryLater();\n"
        "      };\n"
        "      x.onerror=x.ontimeout=function() { i++; next(); };\n"
        "      x.open('GET','http://127.0.0.1:'+ports[i]+'/__update_status',true); x.send();\n"
        "    }\n"
        "    next();\n"
        "  }\n"

        "  function mhManualCheckForUpdates() {\n"
        "    updateStatusFinished=false; updateInstallStarted=false; updatePromptShown=false; lastUpdateToastState=''; manualUpdateCheckActive=true;\n"
        "    showToast('Checking for updates','Contacting GitHub...');\n"
        "    var r=new XMLHttpRequest(); r.timeout=5000;\n"
        "    r.onload=function(){ if(r.status===200){ try { var result=origParse(r.responseText); if(result.accepted){ pollUpdateStatus(); return; } } catch(e){} } manualUpdateCheckActive=false; showToast('Update check failed','Could not start the update check.'); };\n"
        "    r.onerror=r.ontimeout=function(){manualUpdateCheckActive=false; showToast('Update check failed','Could not contact the local updater.');};\n"
        "    r.open('GET','http://127.0.0.1:4040/__manual_update_check',true); r.send();\n"
        "  }\n"
        "\n"
        "  var adBlockEnabled = true;\n"
        "  var origParse = JSON.parse;\n"
        "  JSON.parse = function() {\n"
        "    var r = origParse.apply(this, arguments);\n"
        "    if (adBlockEnabled && r && typeof r === 'object' && !Array.isArray(r)) {\n"
        "      if (r.adPlacements) { delete r.adPlacements; }\n"
        "      if (r.playerAds)    { delete r.playerAds; }\n"
        "      if (r.adSlots)      { delete r.adSlots; }\n"
        "    }\n"
        "    return r;\n"
        "  };\n"
        "\n"
        "  window.JSON.parse = JSON.parse;\n"
        "  try {\n"
        "    for (var key in window._yttv) {\n"
        "      if (window._yttv[key] && window._yttv[key].JSON && window._yttv[key].JSON.parse) {\n"
        "        window._yttv[key].JSON.parse = JSON.parse;\n"
        "      }\n"
        "    }\n"
        "  } catch(e) {}\n"
        "\n"
        "  var blockedCategories = ['__MH_SPONSORBLOCK_CATEGORIES__'];\n"
        "  var skipMode = '__MH_SPONSORBLOCK_SKIP_MODE__';\n"
        "  if (!Array.isArray(blockedCategories) || blockedCategories.length === 0) {\n"
        "    blockedCategories = [];\n"
        "  }\n"
        "  var sponsorBlockEnabled = true;\n"
        "  var sponsorBlockConfigReady = false;\n"
        "  var sponsorSegments = [];\n"
        "  var allSponsorSegments = [];\n"
        "  var currentVideoId = null;\n"
        "  var currentVideo = null;\n"
        "  var skipTimeout = null;\n"
        "  var handledSegments = {};\n"
        "  var lastObservedTime = 0;\n"
        "  var openingRewindGraceUntil = 0;\n"
        "  var activePrompt = null;\n"
        "  var activePromptTimeout = null;\n"
        "\n"
        "  var lastConfigSignature = blockedCategories.join(',') + '|' + skipMode + '|1|1';\n"
        "  var configReloadInProgress = false;\n"
        "  function reloadSponsorBlockConfig() {\n"
        "    if (configReloadInProgress) return;\n"
        "    configReloadInProgress = true;\n"
        "    var ports = [4040,4041,4042,4043,4044,4045,4046,4047,4048,4049,4050];\n"
        "    var tryPort = function(index) {\n"
        "      if (index >= ports.length) { configReloadInProgress = false; return; }\n"
        "      try {\n"
        "        var xhr = new XMLHttpRequest();\n"
        "        xhr.timeout = 1500;\n"
        "        xhr.onload = function() {\n"
        "          if (xhr.status === 200) {\n"
        "            try {\n"
        "              var config = origParse(xhr.responseText);\n"
        "              if (!config || !Array.isArray(config.categories) || (config.skipMode !== 'auto' && config.skipMode !== 'confirm' && config.skipMode !== 'silent')) throw new Error('invalid config');\n"
        "              var wasConfigReady = sponsorBlockConfigReady;\n"
        "              var wasSponsorBlockEnabled = sponsorBlockEnabled;\n"
        "              var nextCategories = config.categories.filter(function(category) { return typeof category === 'string'; });\n"
        "              var nextAdBlockEnabled = config.adBlockEnabled !== false;\n"
        "              var nextSponsorBlockEnabled = config.sponsorBlockEnabled !== false;\n"
        "              var signature = nextCategories.join(',') + '|' + config.skipMode + '|' + (nextAdBlockEnabled ? '1' : '0') + '|' + (nextSponsorBlockEnabled ? '1' : '0');\n"
        "              if (signature !== lastConfigSignature) {\n"
        "                blockedCategories = nextCategories;\n"
        "                skipMode = config.skipMode;\n"
        "                adBlockEnabled = nextAdBlockEnabled;\n"
        "                sponsorBlockEnabled = nextSponsorBlockEnabled;\n"
        "                lastConfigSignature = signature;\n"
        "                dismissSegmentPrompt();\n"
        "                if (!sponsorBlockEnabled) {\n"
        "                  if (skipTimeout) { clearTimeout(skipTimeout); skipTimeout = null; }\n"
        "                  sponsorSegments = [];\n"
        "                } else {\n"
        "                  sponsorSegments = allSponsorSegments.filter(function(segment) { return blockedCategories.includes(segment.category); });\n"
        "                }\n"
        "                scheduleSkip();\n"
        "              }\n"
        "              sponsorBlockConfigReady = true;\n"
        "              if (sponsorBlockEnabled && currentVideoId && (!wasConfigReady || !wasSponsorBlockEnabled)) loadSponsorBlock(currentVideoId);\n"
        "            } catch(e) {}\n"
        "          }\n"
        "          configReloadInProgress = false;\n"
        "        };\n"
        "        xhr.onerror = function() { tryPort(index + 1); };\n"
        "        xhr.ontimeout = function() { tryPort(index + 1); };\n"
        "        xhr.open('GET', 'http://127.0.0.1:' + ports[index] + '/__sponsorblock_config', true);\n"
        "        xhr.send();\n"
        "      } catch(e) { tryPort(index + 1); }\n"
        "    };\n"
        "    tryPort(0);\n"
        "  }\n"
        "  reloadSponsorBlockConfig();\n"
        "\n"
        "  function getVideoId() {\n"
        "    try {\n"
        "      var match = window.location.hash.match(/[?&]v=([^&]+)/);\n"
        "      return match ? match[1] : null;\n"
        "    } catch(e) {\n"
        "      return null;\n"
        "    }\n"
        "  }\n"
        "\n"
        "  function loadSponsorBlock(videoId) {\n"
        "    if (!videoId || !sponsorBlockConfigReady || !sponsorBlockEnabled) return;\n"
        "    var tryPort = function(port) {\n"
        "      try {\n"
        "        var xhr = new XMLHttpRequest();\n"
        "        var url = 'http://127.0.0.1:' + port + '/' + encodeURIComponent(videoId);\n"
        "        xhr.timeout = 2000;\n"
        "        xhr.onload = function() {\n"
        "          if (xhr.status === 200) {\n"
        "            try {\n"
        "              var data = origParse(xhr.responseText);\n"
        "              if (!sponsorBlockEnabled || videoId !== currentVideoId) return;\n"
        "              if (Array.isArray(data)) {\n"
        "                allSponsorSegments = data;\n"
        "                sponsorSegments = allSponsorSegments.filter(function(segment) {\n"
        "                  return blockedCategories.includes(segment.category);\n"
        "                });\n"
        "                scheduleSkip();\n"
        "              } else {\n"
        "                allSponsorSegments = [];\n"
        "                sponsorSegments = [];\n"
        "              }\n"
        "            } catch(e) {}\n"
        "          }\n"
        "        };\n"
        "        xhr.onerror = function() { if (port < 4050) tryPort(port + 1); };\n"
        "        xhr.ontimeout = function() { if (port < 4050) tryPort(port + 1); };\n"
        "        xhr.open('GET', url, true);\n"
        "        xhr.send();\n"
        "      } catch(e) {\n"
        "        if (port < 4050) tryPort(port + 1);\n"
        "      }\n"
        "    };\n"
        "    tryPort(4040);\n"
        "  }\n"
        "\n"
        "  function scheduleSkip() {\n"
        "    if (!sponsorBlockEnabled) { if (skipTimeout) { clearTimeout(skipTimeout); skipTimeout = null; } return; }\n"
        "    // Keep one prompt alive; timeupdate events must not recreate it and reset its timeout.\n"
        "    if (activePrompt) return;\n"
        "    if (skipTimeout) {\n"
        "      clearTimeout(skipTimeout);\n"
        "      skipTimeout = null;\n"
        "    }\n"
        "    if (!currentVideo || currentVideo.paused || sponsorSegments.length === 0) return;\n"
        "    var now = currentVideo.currentTime;\n"
        "    var nextSegments = [];\n"
        "    for (var i = 0; i < sponsorSegments.length; i++) {\n"
        "      var seg = sponsorSegments[i].segment;\n"
        "      var candidate = sponsorSegments[i];\n"
        "      var candidateId = candidate.UUID || (candidate.category + '_' + seg[0] + '_' + seg[1]);\n"
        "      // Include segments that have already started but have not ended yet.\n"
        "      // This matters for segments at 0s when SponsorBlock data arrives slightly late.\n"
        "      if (!handledSegments[candidateId] && seg[1] > now + 0.05) {\n"
        "        nextSegments.push(candidate);\n"
        "      }\n"
        "    }\n"
        "    nextSegments.sort(function(a, b) { return a.segment[0] - b.segment[0]; });\n"
        "    if (nextSegments.length === 0) return;\n"
        "    var segment = nextSegments[0];\n"
        "    var start = segment.segment[0];\n"
        "    var end = segment.segment[1];\n"
        "    var delay = (start - now) * 1000;\n"
        "    // Let a second L1 press register before an opening auto-skip seeks forward.\n"
        "    if (skipMode !== 'confirm' && start < 1 && now < 1 && Date.now() < openingRewindGraceUntil) {\n"
        "      delay = Math.max(delay, openingRewindGraceUntil - Date.now());\n"
        "    }\n"
        "    skipTimeout = setTimeout(function() {\n"
        "      if (!currentVideo || currentVideo.paused) return;\n"
        "      var uuid = segment.UUID || (segment.category + '_' + start + '_' + end);\n"
        "      if (handledSegments[uuid]) { scheduleSkip(); return; }\n"
        "      if (skipMode !== 'confirm') {\n"
        "        // Use the same seek target as confirmation mode, but only report success\n"
        "        // after the browser has accepted the seek.\n"
        "        var videoForSeek = currentVideo;\n"
        "        var targetTime = (isFinite(videoForSeek.duration) && videoForSeek.duration - end < 1) ? Math.max(0, end - 1) : end;\n"
        "        var autoSkipFinished = false;\n"
        "        var finishAutoSkip = function() {\n"
        "          if (autoSkipFinished || videoForSeek !== currentVideo) return;\n"
        "          autoSkipFinished = true;\n"
        "          handledSegments[uuid] = true;\n"
        "          if (skipMode === 'auto') showToast('Segment Skipped', segment.category + ' (' + Math.floor(end - start) + 's)');\n"
        "          videoForSeek.removeEventListener('seeked', finishAutoSkip);\n"
        "          scheduleSkip();\n"
        "        };\n"
        "        try {\n"
        "          videoForSeek.addEventListener('seeked', finishAutoSkip);\n"
        "          videoForSeek.currentTime = targetTime;\n"
        "          if (Math.abs(videoForSeek.currentTime - targetTime) < 0.25 && !videoForSeek.seeking) {\n"
        "            videoForSeek.removeEventListener('seeked', finishAutoSkip);\n"
        "            finishAutoSkip();\n"
        "          } else {\n"
        "            setTimeout(function() {\n"
        "              if (videoForSeek === currentVideo && videoForSeek.currentTime >= targetTime - 0.5) finishAutoSkip();\n"
        "            }, 700);\n"
        "          }\n"
        "        } catch(e) {\n"
        "          videoForSeek.removeEventListener('seeked', finishAutoSkip);\n"
        "          // Seek failed; do not claim the segment was skipped.\n"
        "        }\n"
        "        return;\n"
        "      }\n"
        "      // Confirmation mode: leave playback running and show a temporary prompt.\n"
        "      showSegmentPrompt(segment, uuid, start, end);\n"
        "    }, Math.max(delay, 0));\n"
        "  }\n"
        "\n"
        "  function dismissSegmentPrompt() {\n"
        "    if (activePromptTimeout) { clearTimeout(activePromptTimeout); activePromptTimeout = null; }\n"
        "    if (!activePrompt) return;\n"
        "    if (activePrompt._mhKeyHandler) document.removeEventListener('keydown', activePrompt._mhKeyHandler, true);\n"
        "    if (activePrompt.parentNode) activePrompt.parentNode.removeChild(activePrompt);\n"
        "    activePrompt = null;\n"
        "  }\n"
        "\n"
        "  function markOverlappingSegmentsHandled(start, end) {\n"
        "    var furthestEnd = end;\n"
        "    for (var i = 0; i < sponsorSegments.length; i++) {\n"
        "      var item = sponsorSegments[i];\n"
        "      if (!item || !item.segment || item.segment.length < 2) continue;\n"
        "      var itemStart = item.segment[0];\n"
        "      var itemEnd = item.segment[1];\n"
        "      if (itemStart <= end + 0.05 && itemEnd >= start - 0.05) {\n"
        "        var itemId = item.UUID || (item.category + '_' + itemStart + '_' + itemEnd);\n"
        "        handledSegments[itemId] = true;\n"
        "        if (itemEnd > furthestEnd) furthestEnd = itemEnd;\n"
        "      }\n"
        "    }\n"
        "    return furthestEnd;\n"
        "  }\n"
        "\n"
        "  function resolveSegmentPrompt(segment, uuid, start, end, shouldSkip) {\n"
        "    if (handledSegments[uuid]) { dismissSegmentPrompt(); return; }\n"
        "    var furthestEnd = markOverlappingSegmentsHandled(start, end);\n"
        "    dismissSegmentPrompt();\n"
        "    if (!currentVideo) return;\n"
        "    if (shouldSkip) {\n"
        "      showToast('Segment Skipped', segment.category + ' (' + Math.floor(end - start) + 's)');\n"
        "      try { currentVideo.currentTime = currentVideo.duration - furthestEnd < 1 ? Math.max(0, furthestEnd - 1) : furthestEnd; } catch(e) {}\n"
        "    }\n"
        "    scheduleSkip();\n"
        "  }\n"
        "\n"
        "  function showSegmentPrompt(segment, uuid, start, end) {\n"
        "    dismissSegmentPrompt();\n"
        "    var overlay = document.createElement('div');\n"
        "    overlay.id = 'mh-sponsorblock-confirm';\n"
        "    overlay.style.cssText = 'position:fixed;top:72px;right:72px;z-index:2147483647;display:block;pointer-events:none;font-family:inherit;';\n"
        "    var panel = document.createElement('div');\n"
        "    panel.style.cssText = 'pointer-events:auto;display:inline-block;vertical-align:top;box-sizing:border-box;width:auto;max-width:calc(100vw - 144px);padding:26px 24px;border:0;border-radius:17px;background:rgba(24,24,24,1);color:#fff;text-align:left;box-shadow:none;';\n"
        "    var title = document.createElement('div');\n"
        "    title.textContent = 'SponsorBlock segment';\n"
        "    title.style.cssText = 'font-family:inherit;font-size:30px;font-weight:500;color:#f1f1f1;margin-bottom:14px;text-align:left;position:relative;top:1px;';\n"
        "    var detail = document.createElement('div');\n"
        "    detail.textContent = segment.category + ' (' + Math.max(0, Math.round(end - start)) + 's)';\n"
        "    detail.style.cssText = 'font-family:inherit;font-size:24px;font-weight:400;color:#aaa;text-align:left;margin-bottom:18px;';\n"
        "    var buttons = document.createElement('div');\n"
        "    buttons.style.cssText = 'display:flex;justify-content:center;';\n"
        "    var buttonSpacer = document.createElement('div');\n"
        "    buttonSpacer.style.cssText = 'width:10px;min-width:10px;height:1px;flex:0 0 10px;';\n"
        "    var skipButton = document.createElement('button');\n"
        "    skipButton.type = 'button';\n"
        "    skipButton.textContent = 'Skip segment';\n"
        "    skipButton.style.cssText = '-webkit-appearance:none;appearance:none;font-size:20px;font-weight:bold;padding:12px 18px;border:0;border-radius:12px;outline:none;box-shadow:none;background:#fff;color:#111;';\n"
        "    var keepButton = document.createElement('button');\n"
        "    keepButton.type = 'button';\n"
        "    keepButton.textContent = 'Keep watching';\n"
        "    keepButton.style.cssText = '-webkit-appearance:none;appearance:none;font-size:20px;font-weight:bold;padding:12px 18px;border:0;border-radius:12px;outline:none;box-shadow:none;background:#333;color:#fff;';\n"
        "    var selected = 1;\n"
        "    function updateSelection() {\n"
        "      skipButton.style.background = selected === 1 ? '#fff' : '#333';\n"
        "      skipButton.style.color = selected === 1 ? '#111' : '#fff';\n"
        "      keepButton.style.background = selected === 0 ? '#fff' : '#333';\n"
        "      keepButton.style.color = selected === 0 ? '#111' : '#fff';\n"
        "    }\n"
        "    skipButton.onclick = function(event) { if (event) { event.preventDefault(); event.stopPropagation(); } resolveSegmentPrompt(segment, uuid, start, end, true); };\n"
        "    keepButton.onclick = function(event) { if (event) { event.preventDefault(); event.stopPropagation(); } resolveSegmentPrompt(segment, uuid, start, end, false); };\n"
        "    var keyHandler = function(event) {\n"
        "      if (!activePrompt) return;\n"
        "      var key = event.key || '';\n"
        "      var code = event.keyCode || event.which || 0;\n"
        "      if (key === 'ArrowLeft' || key === 'ArrowRight' || code === 37 || code === 39) {\n"
        "        if (event.preventDefault) event.preventDefault();\n"
        "        if (event.stopPropagation) event.stopPropagation();\n"
        "        selected = selected === 0 ? 1 : 0;\n"
        "        updateSelection();\n"
        "        try { (selected === 1 ? skipButton : keepButton).focus(); } catch(e) {}\n"
        "        return;\n"
        "      }\n"
        "      // Circle / Back dismisses only our prompt; prevent YouTube's Back action.\n"
        "      var isBack = key === 'Escape' || key === 'Backspace' || key === 'o' || key === 'O' || code === 27 || code === 8 || code === 79;\n"
        "      if (isBack) {\n"
        "        if (event.preventDefault) event.preventDefault();\n"
        "        if (event.stopPropagation) event.stopPropagation();\n"
        "        if (event.stopImmediatePropagation) event.stopImmediatePropagation();\n"
        "        markOverlappingSegmentsHandled(start, end);\n"
        "        dismissSegmentPrompt();\n"
        "        if (activePromptTimeout) { clearTimeout(activePromptTimeout); activePromptTimeout = null; }\n"
        "        scheduleSkip();\n"
        "        return;\n"
        "      }\n"
        "      var activate = key === 'Enter' || key === ' ' || key === 'Spacebar' || key === 'x' || key === 'X' || code === 13 || code === 32 || code === 88;\n"
        "      if (activate) {\n"
        "        if (event.preventDefault) event.preventDefault();\n"
        "        if (event.stopPropagation) event.stopPropagation();\n"
        "        if (event.stopImmediatePropagation) event.stopImmediatePropagation();\n"
        "        if (selected === 1) resolveSegmentPrompt(segment, uuid, start, end, true);\n"
        "        else resolveSegmentPrompt(segment, uuid, start, end, false);\n"
        "      }\n"
        "    };\n"
        "    overlay._mhKeyHandler = keyHandler;\n"
        "    panel.appendChild(title);\n"
        "    panel.appendChild(detail);\n"
        "    panel.appendChild(buttons);\n"
        "    buttons.appendChild(keepButton);\n"
        "    buttons.appendChild(buttonSpacer);\n"
        "    buttons.appendChild(skipButton);\n"
        "    overlay.appendChild(panel);\n"
        "    document.body.appendChild(overlay);\n"
        "    activePrompt = overlay;\n"
        "    document.addEventListener('keydown', keyHandler, true);\n"
        "    updateSelection();\n"
        "    try { skipButton.focus(); } catch(e) {}\n"
        "    // Keep the confirmation prompt visible for the segment's full duration.\n"
        "    activePromptTimeout = setTimeout(function() {\n"
        "      resolveSegmentPrompt(segment, uuid, start, end, false);\n"
        "    }, Math.max(0, (end - start) * 1000));\n"
        "  }\n"
        "\n"
        "  function onScheduleSkip() {\n"
        "    if (currentVideo) {\n"
        "      var now = currentVideo.currentTime;\n"
        "      // Detect an actual backward jump, not every timeupdate near the start.\n"
        "      if (now < lastObservedTime - 1) {\n"
        "        if (now < 1) openingRewindGraceUntil = Date.now() + 1400;\n"
        "        if (sponsorSegments.length) {\n"
        "          for (var i = 0; i < sponsorSegments.length; i++) {\n"
        "            var segment = sponsorSegments[i];\n"
        "            var bounds = segment.segment;\n"
        "            var id = segment.UUID || (segment.category + '_' + bounds[0] + '_' + bounds[1]);\n"
        "            var rewindThreshold = bounds[0] < 1 ? bounds[0] + 0.5 : bounds[0] - 1;\n"
        "            if (now < rewindThreshold) delete handledSegments[id];\n"
        "          }\n"
        "        }\n"
        "      }\n"
        "      lastObservedTime = now;\n"
        "    }\n"
        "    scheduleSkip();\n"
        "  }\n"
        "\n"
        "  function detachVideo() {\n"
        "    dismissSegmentPrompt();\n"
        "    if (currentVideo) {\n"
        "      currentVideo.removeEventListener('play', onScheduleSkip);\n"
        "      currentVideo.removeEventListener('pause', onScheduleSkip);\n"
        "      currentVideo.removeEventListener('timeupdate', onScheduleSkip);\n"
        "      currentVideo = null;\n"
        "    }\n"
        "    if (skipTimeout) {\n"
        "      clearTimeout(skipTimeout);\n"
        "      skipTimeout = null;\n"
        "    }\n"
        "  }\n"
        "\n"
        "  function attachVideo() {\n"
        "    detachVideo();\n"
        "    var video = document.querySelector('video');\n"
        "    if (!video) {\n"
        "      setTimeout(attachVideo, 200);\n"
        "      return;\n"
        "    }\n"
        "    currentVideo = video;\n"
        "    currentVideo.addEventListener('play', onScheduleSkip);\n"
        "    currentVideo.addEventListener('pause', onScheduleSkip);\n"
        "    currentVideo.addEventListener('timeupdate', onScheduleSkip);\n"
        "  }\n"
        "\n"
        "  function onVideoChange() {\n"
        "    var videoId = getVideoId();\n"
        "    if (videoId && videoId !== currentVideoId) {\n"
        "      currentVideoId = videoId;\n"
        "      lastObservedTime = 0;\n"
        "      openingRewindGraceUntil = 0;\n"
        "      sponsorSegments = [];\n"
        "      allSponsorSegments = [];\n"
        "      handledSegments = {};\n"
        "      dismissSegmentPrompt();\n"
        "      attachVideo();\n"
        "      loadSponsorBlock(videoId);\n"
        "    }\n"
        "  }\n"
        "\n"
        "  window.addEventListener('hashchange', onVideoChange, false);\n"
        "  onVideoChange();\n"
        "\n"
        "  setTimeout(function() {\n"
        "    showToast('Welcome to AdlessTube', 'By FourZeroCode & earthonion');\n"
        "    setTimeout(pollUpdateStatus, 1000);\n"
        "  }, 2000);\n"
        "  // Options button settings menu. Options is keyCode 179 (MediaPlayPause).\n"
        "  (function installSponsorBlockSettingsShortcut() {\n"
        "    if (window.__ytSbOptionsInstalled) return;\n"
        "    window.__ytSbOptionsInstalled = true;\n"
        "    var categories = [\n"
        "      ['sponsor','Paid sponsorships'],['selfpromo','Creator self-promotion'],['interaction','Like / subscribe reminders'],\n"
        "      ['intro','Video introductions'],['outro','Video endings'],['preview','Previews / teasers'],\n"
        "      ['music_offtopic','Unrelated music sections'],['filler','Filler / tangents'],['hook','Opening hooks'],\n"
        "      ['poi_highlight','Points of interest'],['exclusive_access','Exclusive access promotions'],['chapter','Chapter transitions']\n"
        "    ];\n"
        "    var menu = null, selected = 0, enabled = [], mode = 'auto', busy = false, menuInputGuardUntil = 0;\n"
        "    function isWatchPage() { var route = (window.location.pathname || '') + (window.location.hash || ''); return /(?:^|[#\\/])watch(?:[\\/?&]|$)/i.test(route); }\n"
        "    function suppressMenuEvent(e) { if(e.preventDefault)e.preventDefault(); if(e.stopPropagation)e.stopPropagation(); if(e.stopImmediatePropagation)e.stopImmediatePropagation(); e.cancelBubble=true; if(e.returnValue!==undefined)e.returnValue=false; }\n"
        "    function closeMenu() { menuInputGuardUntil=Date.now()+500; document.removeEventListener('keydown',menuKeyHandler,true); if (menu && menu.parentNode) menu.parentNode.removeChild(menu); menu = null; var backdrop=document.getElementById('yt-sponsorblock-settings-backdrop'); if(backdrop && backdrop.parentNode) backdrop.parentNode.removeChild(backdrop); }\n"
        "    function requestConfig() { return fetch('http://127.0.0.1:4040/__sponsorblock_config', {cache:'no-store'}).then(function(r){ if(!r.ok) throw new Error('config read failed'); return r.json(); }); }\n"
        "    function saveConfig() { var body = JSON.stringify({categories:enabled,skipMode:mode}); return fetch('http://127.0.0.1:4040/__save_sponsorblock_config', {method:'POST',headers:{'Content-Type':'text/plain;charset=UTF-8'},body:body}).then(function(r){if(!r.ok) throw new Error('save failed');return r.json();}).then(function(result){reloadSponsorBlockConfig();return result;}); }\n"
        "    function draw() { if(!menu) return; var rows=menu.querySelectorAll('[data-row]'); for(var i=0;i<rows.length;i++){var row=rows[i];var active=i===selected;var label=i<categories.length?categories[i][1]+'  '+(enabled.indexOf(categories[i][0])>=0?'✓':''):'Skip mode: '+(mode==='auto'?'Auto skip':(mode==='confirm'?'Confirmation':'Auto without notifications'));row.textContent=(active?'▶  ':'   ')+label;row.style.background=active?'#ffffff':'transparent';row.style.color=active?'#111':'#fff';row.style.outline='none';row.style.borderRadius='12px';row.style.fontWeight=active?'bold':'normal';row.style.paddingLeft=active?'18px':'12px';} }\n"
        "    function render() { closeMenu(); var backdrop=document.createElement('div'); backdrop.id='yt-sponsorblock-settings-backdrop'; backdrop.style.cssText='position:fixed;top:0;right:0;bottom:0;left:0;width:100%;height:100%;z-index:2147483646;background-color:rgba(0,0,0,0.82);pointer-events:none;'; document.body.appendChild(backdrop); menu=document.createElement('div'); menu.id='yt-sponsorblock-settings'; menu.style.cssText='position:fixed;top:0;right:0;bottom:0;left:0;width:100%;height:100%;z-index:2147483647;background:transparent;display:flex;align-items:center;justify-content:center;font-family:Roboto-Italic;color:#fff;'; var panel=document.createElement('div'); panel.style.cssText='position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);margin:0;width:660px;max-width:90vw;max-height:88vh;overflow:auto;background:#141414;border-radius:12px;padding:24px 30px;box-shadow:0 5px 30px #000;'; var title=document.createElement('div'); title.textContent='SponsorBlock Settings'; title.style.cssText='font-size:40.5px;font-weight:bold;margin-bottom:14px;text-align:center;'; panel.appendChild(title);\n"
        "      categories.forEach(function(c){var row=document.createElement('div');row.setAttribute('data-row','1');row.style.cssText='padding:9px 12px;font-size:28.5px;border-radius:12px;display:flex;justify-content:space-between;';row.textContent=c[1]+'  '+(enabled.indexOf(c[0])>=0?'✓':'');row._id=c[0];panel.appendChild(row);});\n"
        "      var modeRow=document.createElement('div');modeRow.setAttribute('data-row','1');modeRow.style.cssText='padding:10px 12px;font-size:28.5px;border-radius:12px;margin-top:8px';modeRow.textContent='Skip mode: '+(mode==='auto'?'Auto skip':(mode==='confirm'?'Confirmation':'Auto without notifications'));panel.appendChild(modeRow);\n"
        "      var hint=document.createElement('div');hint.textContent='Changes save automatically    X: toggle    O: close    Up/Down: navigate';hint.style.cssText='text-align:center;color:#bbb;font-size:22.5px;margin-top:12px';panel.appendChild(hint);menu.appendChild(panel);document.body.appendChild(menu);selected=0;document.addEventListener('keydown',menuKeyHandler,true);document.addEventListener('keyup',menuKeyReleaseHandler,true);document.addEventListener('keypress',menuKeyReleaseHandler,true);draw(); }\n"
        "    function openMenu() { if(menu||busy) return; busy=true; requestConfig().then(function(c){enabled=Array.isArray(c.categories)?c.categories.slice():[];mode=(c.skipMode==='confirm'||c.skipMode==='silent')?c.skipMode:'auto';render();}).catch(function(){showToast('SponsorBlock settings','Could not load config.json');}).then(function(){busy=false;}); }\n"
        "    function activate() { if(!menu||busy)return; if(selected<categories.length){var id=categories[selected][0],i=enabled.indexOf(id);if(i>=0)enabled.splice(i,1);else enabled.push(id);} else {mode=mode==='auto'?'confirm':(mode==='confirm'?'silent':'auto');} draw(); busy=true; saveConfig().then(function(){if(menu)draw();}).catch(function(){showToast('Settings save failed','Could not write /data/youtube/config.json');}).then(function(){busy=false;}); }\n"
        "    window.addEventListener('keydown',function(e){var code=e.keyCode||e.which||0;if(code!==179)return;if(menu){if(e.preventDefault)e.preventDefault();if(e.stopPropagation)e.stopPropagation();if(e.stopImmediatePropagation)e.stopImmediatePropagation();e.cancelBubble=true;if(e.returnValue!==undefined)e.returnValue=false;if(!e.repeat)closeMenu();return;}if(isWatchPage())return;if(e.preventDefault)e.preventDefault();if(e.stopPropagation)e.stopPropagation();if(e.stopImmediatePropagation)e.stopImmediatePropagation();if(e.repeat)return;openMenu();},true);\n"
                "    function menuKeyHandler(e) { if(!menu)return; var key=e.key||''; var code=e.keyCode||e.which||0; suppressMenuEvent(e); if(code===32782||code===32787||code===38||key==='ArrowUp'){selected=(selected+categories.length)%(categories.length+1);draw();return;} if(code===32783||code===32788||code===40||key==='ArrowDown'){selected=(selected+1)%(categories.length+1);draw();return;} if(key==='o'||key==='O'||key==='Escape'||key==='Backspace'||code===79||code===27||code===8||code===32769){closeMenu();return;} if(code===32768||code===88||code===13||key==='x'||key==='X'||key==='Enter'||key===' '){if(!e.repeat)activate();return;} }\n"
        "    function menuKeyReleaseHandler(e) { if(menu || Date.now()<menuInputGuardUntil)suppressMenuEvent(e); }\n"
                "    // Native SponsorBlock submenu: same popup renderer used by YouTube's Quality panel.\n"
        "    var MH_ACTION='MH_YT_SB_OPEN_SETTINGS';\n"
        "    var MH_OPEN_SB_SETTINGS='MH_YT_SB_OPEN_SPONSORBLOCK_SETTINGS';\n"
        "    var MH_OPEN_UPDATE_CENTER='MH_YT_SB_OPEN_UPDATE_CENTER';\n"
        "    var MH_CHECK_UPDATES='MH_YT_SB_CHECK_UPDATES';\n"
        "    var MH_TOGGLE_STARTUP_UPDATE_CHECK='MH_YT_SB_TOGGLE_STARTUP_UPDATE_CHECK';\n"
        "    var MH_TOGGLE_ADBLOCK='MH_YT_SB_TOGGLE_ADBLOCK';\n"
        "    var MH_TOGGLE_SPONSORBLOCK='MH_YT_SB_TOGGLE_SPONSORBLOCK';\n"
        "    var MH_TOGGLE='MH_YT_SB_TOGGLE_CATEGORY';\n"
        "    var MH_MODE='MH_YT_SB_TOGGLE_SKIP_MODE';\n"
        "    var MH_SELECT_MODE='MH_YT_SB_SELECT_SKIP_MODE';\n"
        "    var mhNativeResolve=null;\n"
        "    var mhCategoryLabels={\n"
        "      sponsor:'Sponsor',intro:'Intro',outro:'Outro',selfpromo:'Self promotion',\n"
        "      interaction:'Interaction reminder',music_offtopic:'Non-music section',\n"
        "      preview:'Preview / recap',filler:'Filler',exclusive_access:'Exclusive access',\n"
        "      poi_highlight:'POI highlight',chapter:'Chapter',hook:'Hook'\n"
        "    };\n"
        "    function mhText(v){return v&&((v.simpleText)||((v.runs||[]).map(function(r){return r.text||'';}).join('')))||'';}\n"
        "    // Main TV Settings entries arrive as JSON API data, not popup commands.\n"
        "    // Patch JSON.parse (the same response-interception point used by the existing\n"
        "    // ad-removal code) before YouTube builds the Settings tabs.\n"
        "    var MH_SETTINGS_ITEM_ID='YT_SPONSORBLOCK_SETTINGS';\n"
        "    function mhPatchSettingsData(root){\n"
        "      if(!root||typeof root!=='object')return root;\n"
        "      var seen=[];\n"
        "      function visit(node,depth){\n"
        "        if(!node||typeof node!=='object'||depth>24||seen.indexOf(node)>=0)return;\n"
        "        seen.push(node);\n"
        "        if(Array.isArray(node)){\n"
        "          for(var i=0;i<node.length;i++){\n"
        "            var collection=node[i]&&node[i].settingCategoryCollectionRenderer;\n"
        "            if(collection&&Array.isArray(collection.items)){\n"
        "              var items=collection.items,restricted=-1,exists=false;\n"
        "              for(var j=0;j<items.length;j++){\n"
        "                var entry=items[j]||{},action=entry.settingActionRenderer,booleanSetting=entry.settingBooleanRenderer;\n"
        "                var id=(action&&action.itemId)||(booleanSetting&&booleanSetting.itemId)||'';\n"
        "                var label=mhText((action&&action.title)||(booleanSetting&&booleanSetting.title));\n"
        "                if(id===MH_SETTINGS_ITEM_ID||label==='AdlessTube')exists=true;\n"
        "                if(label==='Restricted mode')restricted=j;\n"
        "              }\n"
        "              if(!exists&&restricted>=0){\n"
        "                items.splice(restricted+1,0,{settingActionRenderer:{\n"
        "                  title:{runs:[{text:'AdlessTube'}]},\n"
        "                  summary:{runs:[{text:'Configure AdlessTube settings'}]},\n"
        "                  itemId:MH_SETTINGS_ITEM_ID,\n"
        "                  serviceEndpoint:{signalAction:{customAction:{action:MH_ACTION,parameters:[]}}}\n"
        "                }});\n"
        "                exists=true;\n"
        "              }\n"
        "            }\n"
        "            visit(node[i],depth+1);\n"
        "          }\n"
        "          return;\n"
        "        }\n"
        "        for(var key in node)if(Object.prototype.hasOwnProperty.call(node,key))visit(node[key],depth+1);\n"
        "      }\n"
        "      visit(root,0);return root;\n"
        "    }\n"
        "    function mhInstallSettingsJsonHook(){\n"
        "      function patchJsonObject(jsonObject){\n"
        "        if(!jsonObject||typeof jsonObject.parse!=='function'||jsonObject.parse.__mhSbSettingsPatched)return false;\n"
        "        var originalParse=jsonObject.parse;\n"
        "        var wrappedParse=function(){var parsed=originalParse.apply(this,arguments);try{mhPatchSettingsData(parsed);}catch(e){}return parsed;};\n"
        "        wrappedParse.__mhSbSettingsPatched=true;jsonObject.parse=wrappedParse;return true;\n"
        "      }\n"
        "      var patched=patchJsonObject(window.JSON);\n"
        "      try{for(var key in window._yttv){var ctx=window._yttv[key];if(ctx&&ctx.JSON)patched=patchJsonObject(ctx.JSON)||patched;}}catch(e){}\n"
        "      return patched;\n"
        "    }\n"
        "    mhInstallSettingsJsonHook();\n"
        "    var mhJsonHookAttempts=0;var mhJsonHookTimer=setInterval(function(){mhJsonHookAttempts++;if(mhInstallSettingsJsonHook()||mhJsonHookAttempts>=80)clearInterval(mhJsonHookTimer);},250);\n"
        "    function mhOpenPopup(resolve, config, updateExisting){\n"
        "      if(!resolve)return;\n"
        "      var enabled=(config&&Array.isArray(config.categories))?config.categories:[];\n"
        "      var ids=Object.keys(mhCategoryLabels);\n"
        "      var items=ids.map(function(id){\n"
        "        var active=enabled.indexOf(id)>=0;\n"
        "        return {compactLinkRenderer:{\n"
        "          title:{runs:[{text:mhCategoryLabels[id]}]},\n"
        "          subtitle:{simpleText:id},\n"
        "          secondaryIcon:active?{iconType:'CHECK'}:undefined,\n"
        "          serviceEndpoint:{signalAction:{customAction:{action:MH_TOGGLE,parameters:[id]}}}\n"
        "        }};\n"
        "      });\n"
        "      var currentMode=(config&&(config.skipMode==='confirm'||config.skipMode==='silent'))?config.skipMode:'auto';\n"
        "      items.unshift({compactLinkRenderer:{\n"
        "        title:{runs:[{text:'Segment skip mode'}]},\n"
        "        icon:{iconType:'SKIP_NEXT'},\n"
        "        subtitle:{simpleText:currentMode==='auto'?'Auto skip':(currentMode==='confirm'?'Ask for confirmation':'Auto without notifications')},\n"
        "        secondaryIcon:{iconType:'CHEVRON_RIGHT'},\n"
        "        serviceEndpoint:{signalAction:{customAction:{action:MH_MODE,parameters:[]}}}\n"
        "      }});\n"
        "      var popup={\n"
        "        openPopupAction:{\n"
        "          popupType:'MODAL',\n"
        "          popup:{\n"
        "            overlaySectionRenderer:{\n"
        "              overlay:{\n"
        "                overlayTwoPanelRenderer:{\n"
        "                  actionPanel:{\n"
        "                    overlayPanelRenderer:{\n"
        "                      header:{overlayPanelHeaderRenderer:{title:{runs:[{text:'SponsorBlock'}]}}},\n"
        "                      content:{overlayPanelItemListRenderer:{items:items}}\n"
        "                    }\n"
        "                  }\n"
        "                }\n"
        "              }\n"
        "            }\n"
        "          },\n"
        "          backButton:{signalAction:{signal:'POPUP_BACK'}},\n"
        "          dismissalCommand:{signalAction:{signal:'POPUP_BACK'}},\n"
        "          uniqueId:'yt-sponsorblock-settings',\n"
        "          selectedIndex:0\n"
        "        }\n"
        "      };\n"
        "      if(updateExisting){popup.openPopupAction.updateAction=true;popup.openPopupAction.shouldMatchUniqueId=true;}\n"
        "      resolve(popup);\n"
        "    }\n"
        "    function mhGetConfig(){\n"
        "      return fetch('http://127.0.0.1:4040/__sponsorblock_config',{cache:'no-store'})\n"
        "        .then(function(r){if(!r.ok)throw new Error('config read failed');return r.json();});\n"
        "    }\n"
        "    function mhSaveConfig(config){\n"
        "      return fetch('http://127.0.0.1:4040/__save_sponsorblock_config',{\n"
        "        method:'POST',headers:{'Content-Type':'text/plain;charset=UTF-8'},\n"
        "        body:JSON.stringify(config)\n"
        "      }).then(function(r){if(!r.ok)throw new Error('config save failed');return r.json();}).then(function(result){reloadSponsorBlockConfig();return result;});\n"
        "    }\n"
        "    function mhOpenAddonPopup(resolve,updateExisting){\n"
        "      mhGetConfig().then(function(config){\n"
        "        var adBlockEnabled=config.adBlockEnabled!==false;\n"
        "        var adBlockItem={compactLinkRenderer:{\n"
        "          title:{runs:[{text:'AdBlock'}]},\n"
        "          icon:{iconType:'PAID'},\n"
        "          subtitle:{simpleText:adBlockEnabled?'Enabled':'Disabled'},\n"
        "          secondaryIcon:{iconType:adBlockEnabled?'CHECK_BOX':'CHECK_BOX_OUTLINE_BLANK'},\n"
        "          serviceEndpoint:{signalAction:{customAction:{action:MH_TOGGLE_ADBLOCK,parameters:[]}}}\n"
        "        }};\n"
        "        var sponsorBlockEnabled=config.sponsorBlockEnabled!==false;\n"
        "        var sponsorBlockToggleItem={compactLinkRenderer:{\n"
        "          title:{runs:[{text:'SponsorBlock'}]},\n"
        "          icon:{iconType:'LIST'},\n"
        "          subtitle:{simpleText:sponsorBlockEnabled?'Enabled':'Disabled'},\n"
        "          secondaryIcon:{iconType:sponsorBlockEnabled?'CHECK_BOX':'CHECK_BOX_OUTLINE_BLANK'},\n"
        "          serviceEndpoint:{signalAction:{customAction:{action:MH_TOGGLE_SPONSORBLOCK,parameters:[]}}}\n"
        "        }};\n"
        "        var item={compactLinkRenderer:{\n"
        "          title:{runs:[{text:'SponsorBlock settings'}]},\n"
        "          subtitle:{simpleText:'Skip mode and category settings'},\n"
        "          icon:{iconType:'SETTINGS'},\n"
        "          secondaryIcon:{iconType:'CHEVRON_RIGHT'},\n"
        "          serviceEndpoint:{signalAction:{customAction:{action:MH_OPEN_SB_SETTINGS,parameters:[]}}}\n"
        "        }};\n"
        "        var updateItem={compactLinkRenderer:{\n"
        "          title:{runs:[{text:'Update Center'}]},\n"
        "          subtitle:{simpleText:'Update and startup update settings'},\n"
        "          icon:{iconType:'SORT'},\n"
        "          secondaryIcon:{iconType:'CHEVRON_RIGHT'},\n"
        "          serviceEndpoint:{signalAction:{customAction:{action:MH_OPEN_UPDATE_CENTER,parameters:[]}}}\n"
        "        }};\n"
        "        var popup={\n"
        "          openPopupAction:{\n"
        "            popupType:'MODAL',\n"
        "            popup:{\n"
        "              overlaySectionRenderer:{\n"
        "                overlay:{\n"
        "                  overlayTwoPanelRenderer:{\n"
        "                    actionPanel:{\n"
        "                      overlayPanelRenderer:{\n"
        "                        header:{overlayPanelHeaderRenderer:{title:{runs:[{text:'AdlessTube'}]}}},\n"
        "                        content:{overlayPanelItemListRenderer:{items:[adBlockItem,sponsorBlockToggleItem,item,updateItem]}}\n"
        "                      }\n"
        "                    }\n"
        "                  }\n"
        "                }\n"
        "              }\n"
        "            },\n"
        "            backButton:{signalAction:{signal:'POPUP_BACK'}},\n"
        "            dismissalCommand:{signalAction:{signal:'POPUP_BACK'}},\n"
        "            uniqueId:'ps4-yt-adsponsorblock',\n"
        "            selectedIndex:0\n"
        "          }\n"
        "        };\n"
        "        if(updateExisting){popup.openPopupAction.updateAction=true;popup.openPopupAction.shouldMatchUniqueId=true;}\n"
        "        resolve(popup);\n"
        "      }).catch(function(){\n"
        "        var fallback={openPopupAction:{popupType:'MODAL',popup:{overlaySectionRenderer:{overlay:{overlayTwoPanelRenderer:{actionPanel:{overlayPanelRenderer:{header:{overlayPanelHeaderRenderer:{title:{runs:[{text:'AdlessTube'}]}}},content:{overlayPanelItemListRenderer:{items:[]}}}}}}}},backButton:{signalAction:{signal:'POPUP_BACK'}},dismissalCommand:{signalAction:{signal:'POPUP_BACK'}},uniqueId:'ps4-yt-adsponsorblock',selectedIndex:0}};\n"
        "        if(updateExisting){fallback.openPopupAction.updateAction=true;fallback.openPopupAction.shouldMatchUniqueId=true;}\n"
        "        resolve(fallback);\n"
        "      });\n"
        "    }\n"
        "    function mhOpenUpdateCenter(resolve,updateExisting){\n"
        "      mhGetConfig().then(function(config){\n"
        "        var enabled=config.checkUpdatesOnStartup!==false;\n"
        "        var items=[\n"
        "          {compactLinkRenderer:{\n"
        "            title:{runs:[{text:'Check for updates'}]},\n"
        "            subtitle:{simpleText:'Manually check for a new version'},\n"
        "            serviceEndpoint:{signalAction:{customAction:{action:MH_CHECK_UPDATES,parameters:[]}}}\n"
        "          }},\n"
        "          {compactLinkRenderer:{\n"
        "            title:{runs:[{text:'Check for updates on startup'}]},\n"
        "            subtitle:{simpleText:enabled?'Enabled':'Disabled'},\n"
        "            secondaryIcon:{iconType:enabled?'CHECK_BOX':'CHECK_BOX_OUTLINE_BLANK'},\n"
        "            serviceEndpoint:{signalAction:{customAction:{action:MH_TOGGLE_STARTUP_UPDATE_CHECK,parameters:[]}}}\n"
        "          }}\n"
        "        ];\n"
        "        var popup={openPopupAction:{popupType:'MODAL',popup:{overlaySectionRenderer:{overlay:{overlayTwoPanelRenderer:{actionPanel:{overlayPanelRenderer:{header:{overlayPanelHeaderRenderer:{title:{runs:[{text:'Update Center'}]}}},content:{overlayPanelItemListRenderer:{items:items}}}}}}}},backButton:{signalAction:{signal:'POPUP_BACK'}},dismissalCommand:{signalAction:{signal:'POPUP_BACK'}},uniqueId:'ps4-yt-adsponsorblock-update-center',selectedIndex:0}};\n"
        "        if(updateExisting){popup.openPopupAction.updateAction=true;popup.openPopupAction.shouldMatchUniqueId=true;}\n"
        "        resolve(popup);\n"
        "      }).catch(function(){\n"
        "        var popup={openPopupAction:{popupType:'MODAL',popup:{overlaySectionRenderer:{overlay:{overlayTwoPanelRenderer:{actionPanel:{overlayPanelRenderer:{header:{overlayPanelHeaderRenderer:{title:{runs:[{text:'Update Center'}]}}},content:{overlayPanelItemListRenderer:{items:[]}}}}}}}},backButton:{signalAction:{signal:'POPUP_BACK'}},dismissalCommand:{signalAction:{signal:'POPUP_BACK'}},uniqueId:'ps4-yt-adsponsorblock-update-center',selectedIndex:0}};\n"
        "        resolve(popup);\n"
        "      });\n"
        "    }\n"
        "    function mhToggleStartupUpdateCheck(resolve){\n"
        "      mhGetConfig().then(function(config){config.checkUpdatesOnStartup=config.checkUpdatesOnStartup===false;return mhSaveConfig(config);})\n"
        "        .then(function(){return mhGetConfig();})\n"
        "        .then(function(config){mhOpenUpdateCenter(resolve,true);})\n"
        "        .catch(function(){try{mhGetConfig().then(function(config){mhOpenUpdateCenter(resolve,true);});}catch(e){}});\n"
        "      return true;\n"
        "    }\n"
        "    function mhToggleAdBlock(resolve){\n"
        "      mhGetConfig().then(function(config){config.adBlockEnabled=config.adBlockEnabled===false;return mhSaveConfig(config);})\n"
        "        .then(function(){return mhGetConfig();})\n"
        "        .then(function(config){mhOpenAddonPopup(resolve,true);})\n"
        "        .catch(function(){try{mhGetConfig().then(function(config){mhOpenAddonPopup(resolve,true);});}catch(e){}});\n"
        "      return true;\n"
        "    }\n"
        "    function mhToggleSponsorBlock(resolve){\n"
        "      mhGetConfig().then(function(config){config.sponsorBlockEnabled=config.sponsorBlockEnabled===false;return mhSaveConfig(config);})\n"
        "        .then(function(){return mhGetConfig();})\n"
        "        .then(function(config){mhOpenAddonPopup(resolve,true);})\n"
        "        .catch(function(){try{mhGetConfig().then(function(config){mhOpenAddonPopup(resolve,true);});}catch(e){}});\n"
        "      return true;\n"
        "    }\n"
        "    function mhOpenNative(resolve){\n"
        "      mhNativeResolve=resolve;\n"
        "      mhGetConfig().then(function(config){mhOpenPopup(resolve,config);})\n"
        "        .catch(function(){mhOpenPopup(resolve,{categories:[]});});\n"
        "    }\n"
        "    function mhToggleCategory(resolve,id){\n"
        "      if(!id||!mhCategoryLabels[id])return true;\n"
        "      mhGetConfig().then(function(config){\n"
        "        var enabled=Array.isArray(config.categories)?config.categories.slice():[];\n"
        "        var at=enabled.indexOf(id);\n"
        "        if(at>=0)enabled.splice(at,1);else enabled.push(id);\n"
        "        config.categories=enabled;\n"
        "        return mhSaveConfig(config);\n"
        "      }).then(function(){\n"
        "        return mhGetConfig();\n"
        "      }).then(function(config){mhOpenPopup(resolve,config,true);})\n"
        "        .catch(function(){try{mhGetConfig().then(function(config){mhOpenPopup(resolve,config,true);});}catch(e){}});\n"
        "      return true;\n"
        "    }\n"
        "    function mhOpenModePopup(resolve,config,updateExisting){\n"
        "      var current=(config&&(config.skipMode==='confirm'||config.skipMode==='silent'))?config.skipMode:'auto';\n"
        "      var choices=[['auto','Auto skip'],['silent','Auto without notifications'],['confirm','Ask for confirmation']];\n"
        "      var selectedIndex=0;\n"
        "      for(var i=0;i<choices.length;i++){if(current===choices[i][0]){selectedIndex=i;break;}}\n"
        "      var items=choices.map(function(choice){return {compactLinkRenderer:{\n"
        "        title:{runs:[{text:choice[1]}]},\n"
        "        secondaryIcon:{iconType:current===choice[0]?'CHECK_BOX':'CHECK_BOX_OUTLINE_BLANK'},\n"
        "        serviceEndpoint:{signalAction:{customAction:{action:MH_SELECT_MODE,parameters:[choice[0]]}}}\n"
        "      }};});\n"
        "      var popup={openPopupAction:{popupType:'MODAL',popup:{overlaySectionRenderer:{overlay:{overlayTwoPanelRenderer:{actionPanel:{overlayPanelRenderer:{header:{overlayPanelHeaderRenderer:{title:{runs:[{text:'Segment skip mode'}]}}},content:{overlayPanelItemListRenderer:{items:items,selectedIndex:selectedIndex}}}}}}}},backButton:{signalAction:{signal:'POPUP_BACK'}},dismissalCommand:{signalAction:{signal:'POPUP_BACK'}},uniqueId:'yt-sponsorblock-skip-mode'}};\n"
        "      if(updateExisting){popup.openPopupAction.updateAction=true;popup.openPopupAction.shouldMatchUniqueId=true;}\n"
        "      resolve(popup);\n"
        "    }\n"
        "    function mhOpenSkipMode(resolve){\n"
        "      mhGetConfig().then(function(config){mhOpenModePopup(resolve,config);})\n"
        "        .catch(function(){mhOpenModePopup(resolve,{skipMode:'auto'});});\n"
        "      return true;\n"
        "    }\n"
        "    function mhSelectSkipMode(resolve,mode){\n"
        "      if(['auto','confirm','silent'].indexOf(mode)<0)return true;\n"
        "      mhGetConfig().then(function(config){\n"
        "        config.skipMode=mode;\n"
        "        return mhSaveConfig(config);\n"
        "      }).then(function(){return mhGetConfig();})\n"
        "        .then(function(config){\n"
        "          resolve({commandExecutorCommand:{commands:[{signalAction:{signal:'POPUP_BACK'}}]}});\n"
        "          mhOpenPopup(resolve,config,true);\n"
        "        })\n"
        "        .catch(function(){try{mhGetConfig().then(function(config){resolve({commandExecutorCommand:{commands:[{signalAction:{signal:'POPUP_BACK'}}]}});mhOpenPopup(resolve,config,true);});}catch(e){}});\n"
        "      return true;\n"
        "    }\n"
        "    function mhFindArrays(node,depth,out){\n"
        "      if(!node||typeof node!=='object'||depth>12)return;\n"
        "      if(Array.isArray(node)){\n"
        "        var stats=false,addon=false;\n"
        "        for(var i=0;i<node.length;i++){var cr=node[i]&&node[i].compactLinkRenderer;if(cr){var t=mhText(cr.title);if(t==='Stats for nerds')stats=true;if(t==='AdlessTube')addon=true;}}\n"
        "        if(stats&&!addon)out.push(node);\n"
        "        for(var j=0;j<node.length;j++)mhFindArrays(node[j],depth+1,out);\n"
        "        return;\n"
        "      }\n"
        "      for(var k in node)if(Object.prototype.hasOwnProperty.call(node,k))mhFindArrays(node[k],depth+1,out);\n"
        "    }\n"
        "    function mhPatchPopup(cmd){\n"
        "      if(!cmd||!cmd.openPopupAction||!cmd.openPopupAction.popup)return;\n"
        "      var arrays=[];mhFindArrays(cmd.openPopupAction.popup,0,arrays);\n"
        "      for(var a=0;a<arrays.length;a++){\n"
        "        var arr=arrays[a],already=false,statsIndex=-1;\n"
        "        for(var i=0;i<arr.length;i++){var cr=arr[i]&&arr[i].compactLinkRenderer;if(cr){var label=mhText(cr.title);if(label==='AdlessTube')already=true;if(label==='Stats for nerds')statsIndex=i;}}\n"
        "        if(already||statsIndex<0)continue;\n"
        "        arr.splice(statsIndex+1,0,{compactLinkRenderer:{title:{simpleText:'AdlessTube'},icon:{iconType:'NOT_INTERESTED'},secondaryIcon:{iconType:'CHEVRON_RIGHT'},serviceEndpoint:{signalAction:{customAction:{action:MH_ACTION,parameters:[]}}}}});\n"
        "      }\n"
        "    }\n"
        "    function mhInstallCommandHook(){\n"
        "      var table=window._yttv;if(!table)return false;var didPatch=false;\n"
        "      for(var key in table){\n"
        "        var inst=table[key]&&table[key].instance;\n"
        "        if(!inst||typeof inst.resolveCommand!=='function'||inst.resolveCommand.__mhSbPatched)continue;\n"
        "        (function(target,original){\n"
        "          var dispatch=function(cmd){return original.call(target,cmd);};\n"
        "          var wrapped=function(cmd){\n"
        "            try{\n"
        "              var custom=(cmd&&cmd.customAction)||(cmd&&cmd.signalAction&&cmd.signalAction.customAction);\n"
        "              if(custom&&custom.action===MH_ACTION){mhOpenAddonPopup(dispatch);return true;}if(custom&&custom.action===MH_TOGGLE_ADBLOCK){return mhToggleAdBlock(dispatch);}if(custom&&custom.action===MH_TOGGLE_SPONSORBLOCK){return mhToggleSponsorBlock(dispatch);}if(custom&&custom.action===MH_OPEN_SB_SETTINGS){mhOpenNative(dispatch);return true;}\n              if(custom&&custom.action===MH_OPEN_UPDATE_CENTER){mhOpenUpdateCenter(dispatch);return true;}if(custom&&custom.action===MH_CHECK_UPDATES){mhManualCheckForUpdates();return true;}if(custom&&custom.action===MH_TOGGLE_STARTUP_UPDATE_CHECK){return mhToggleStartupUpdateCheck(dispatch);}\n"
        "              if(custom&&custom.action===MH_TOGGLE){return mhToggleCategory(dispatch,custom.parameters&&custom.parameters[0]);}\n"
        "              if(custom&&custom.action===MH_MODE){return mhOpenSkipMode(dispatch);}\n"
        "              if(custom&&custom.action===MH_SELECT_MODE){return mhSelectSkipMode(dispatch,custom.parameters&&custom.parameters[0]);}\n"
        "              mhPatchPopup(cmd);\n"
        "            }catch(err){}\n"
        "            return original.apply(this,arguments);\n"
        "          };\n"
        "          wrapped.__mhSbPatched=true;target.resolveCommand=wrapped;\n"
        "        })(inst,inst.resolveCommand);didPatch=true;\n"
        "      }\n"
        "      return didPatch;\n"
        "    }\n"
        "    var mhPatchAttempts=0;\n"
        "    var mhPatchTimer=setInterval(function(){mhPatchAttempts++;if(mhInstallCommandHook()||mhPatchAttempts>=80)clearInterval(mhPatchTimer);},250);\n"
        "    mhInstallCommandHook();\n"
        "    window.addEventListener('yt-navigate-finish',mhInstallCommandHook,false);\n"
"  })();\n"
        "})();\n";

    g_js_payload.assign(kHardcodedJs, kHardcodedJs + sizeof(kHardcodedJs) - 1);

    // Load categories once and substitute them into the embedded JavaScript.
    std::string skip_mode = "auto";
    std::vector<std::string> categories = load_categories(&skip_mode);
    // Missing, unreadable, empty, or unrecognized config returns kDefaultCategories
    // (empty by default, so SponsorBlock skips no categories).
    if (categories.empty()) {
        MH_LOG("Category loader returned an empty list; forcibly applying defaults");
        categories = kDefaultCategories;
    }
    MH_LOG("Active SponsorBlock categories (%zu): %s",
        categories.size(), categories_to_js_array(categories).c_str());
    // Replace the unique category marker with the configured list. If the marker
    // is absent, the JavaScript fallback remains an empty category list.

    const std::string default_js_list =
        "['__MH_SPONSORBLOCK_CATEGORIES__']";
    const size_t list_pos = g_js_payload.find(default_js_list);
    if (list_pos != std::string::npos) {
        g_js_payload.replace(list_pos, default_js_list.size(),
            categories_to_js_array(categories));
        MH_LOG("Injected SponsorBlock category array into embedded JS");
    }
    else {
        MH_LOG("Could not find category marker in embedded JS; SponsorBlock categories remain empty");
    }

    const std::string mode_marker = "__MH_SPONSORBLOCK_SKIP_MODE__";
    const size_t mode_pos = g_js_payload.find(mode_marker);
    if (mode_pos != std::string::npos) {
        g_js_payload.replace(mode_pos, mode_marker.size(), skip_mode);
        MH_LOG("Injected SponsorBlock skip mode: %s", skip_mode.c_str());
    }
    else {
        MH_LOG("Skip-mode marker missing; JavaScript defaults to automatic mode");
    }

    g_js_payload_loaded = true;
    MH_LOG("[execute.inject] loaded hardcoded payload %zu bytes", g_js_payload.size());
    return true;
}

const std::string& get_js_payload() {
    return g_js_payload;
}

void reset_js_payload_state() {
    g_js_payload.clear();
    g_js_payload_loaded = false;
}

void clear_script_cache() {
    // Empty stub - kept for API compatibility
}

void clear_context_tracking() {
    // Empty stub - kept for API compatibility
}

static uint32_t pattern_to_byte(const char* pattern, uint8_t* bytes) {
    uint32_t count = 0;
    const char* start = pattern;
    const char* end = pattern + strlen(pattern);

    for (const char* current = start; current < end; ++current) {
        if (*current == '?') {
            ++current;
            if (*current == '?') {
                ++current;
            }
            bytes[count++] = 0xff;
        }
        else {
            bytes[count++] = strtoul(current, (char**)&current, 16);
        }
    }
    return count;
}

uint8_t* pattern_scan(uint64_t module_base, uint32_t module_size, const char* signature) {
    if (!module_base || !module_size) {
        return nullptr;
    }

    constexpr uint32_t MAX_PATTERN_LENGTH = 256;
    uint8_t patternBytes[MAX_PATTERN_LENGTH] = { 0 };
    int32_t patternLength = pattern_to_byte(signature, patternBytes);

    if (!patternLength || patternLength >= MAX_PATTERN_LENGTH) {
        MH_LOG("Pattern length too large or invalid! %d", patternLength);
        return nullptr;
    }

    mh_log("[pattern_scan] Scanning module base=0x%lx size=0x%x pattern_len=%d\n",
        module_base, module_size, patternLength);
    mh_log("[pattern_scan] First 16 pattern bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
        patternBytes[0], patternBytes[1], patternBytes[2], patternBytes[3],
        patternBytes[4], patternBytes[5], patternBytes[6], patternBytes[7],
        patternBytes[8], patternBytes[9], patternBytes[10], patternBytes[11],
        patternBytes[12], patternBytes[13], patternBytes[14], patternBytes[15]);

    uint8_t* scanBytes = (uint8_t*)module_base;

    for (uint64_t i = 0; i < module_size; ++i) {
        bool found = true;
        for (int32_t j = 0; j < patternLength; ++j) {
            if (scanBytes[i + j] != patternBytes[j] && patternBytes[j] != 0xff) {
                found = false;
                break;
            }
        }
        if (found) {
            mh_log("[pattern_scan] Match found at offset 0x%lx (addr 0x%lx)\n", i, module_base + i);
            mh_log("[pattern_scan] First 16 bytes at match: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                scanBytes[i], scanBytes[i + 1], scanBytes[i + 2], scanBytes[i + 3],
                scanBytes[i + 4], scanBytes[i + 5], scanBytes[i + 6], scanBytes[i + 7],
                scanBytes[i + 8], scanBytes[i + 9], scanBytes[i + 10], scanBytes[i + 11],
                scanBytes[i + 12], scanBytes[i + 13], scanBytes[i + 14], scanBytes[i + 15]);
            return &scanBytes[i];
        }
    }
    return nullptr;
}

bool try_extract_script_from_candidate(uint64_t candidate, std::string& out,
    StringLikeLayout* out_layout) {
    out.clear();
    if (out_layout) {
        out_layout->valid = false;
    }
    if (!candidate || !is_canonical_address(candidate)) {
        return false;
    }

    constexpr size_t kProbe = 0x100;
    std::array<uint8_t, kProbe> header{};
    sys_proc_ro(candidate, header.data(), header.size());

    auto read_u64 = [&](size_t offset) -> uint64_t {
        if (offset + sizeof(uint64_t) > header.size()) {
            return 0;
        }
        uint64_t value = 0;
        std::memcpy(&value, header.data() + offset, sizeof(value));
        return value;
        };

    auto try_pointer_and_length = [&](size_t d_off, size_t n_off, size_t c_off) -> bool {
        uint64_t data_ptr = read_u64(d_off);
        uint64_t len = read_u64(n_off);
        uint64_t cap = read_u64(c_off);
        if (!data_ptr || !is_canonical_address(data_ptr)) {
            return false;
        }
        if (len == 0 || len > (1ULL << 20)) {
            return false;
        }
        if (cap < len || cap >(1ULL << 20)) {
            return false;
        }
        std::vector<uint8_t> temp = read_remote_buffer(data_ptr, static_cast<size_t>(len));
        if (temp.empty()) {
            return false;
        }
        size_t sample = std::min<size_t>(temp.size(), 512);
        if (!looks_printable_text(temp.data(), sample, 0.55)) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(temp.data()), temp.size());
        if (out_layout) {
            out_layout->valid = true;
            out_layout->data_off = d_off;
            out_layout->size_off = n_off;
            out_layout->cap_off = c_off;
            out_layout->header_span = std::max<size_t>(0x40, std::max({ d_off, n_off, c_off }) + 8);
            MH_LOG("[extract.layout] found at data@0x%zx size@0x%zx cap@0x%zx ptr=%p len=%llu cap=%llu",
                d_off, n_off, c_off, (void*)data_ptr,
                static_cast<unsigned long long>(len), static_cast<unsigned long long>(cap));
        }
        return true;
        };

    // Try common offsets for pointer+size+cap pattern
    for (size_t base = 0; base + 24 <= 0x80; base += 8) {
        if (try_pointer_and_length(base, base + 8, base + 16)) {
            return true;
        }
    }

    // Try additional specific offsets
    const size_t offsets[] = { 0x18, 0x20, 0x28, 0x30, 0x38,
                              0x40, 0x48, 0x50, 0x58, 0x60,
                              0x68, 0x70, 0x78, 0x80, 0x88,
                              0x90, 0x98, 0xa0, 0xa8, 0xb0,
                              0xb8, 0xc0, 0xc8, 0xd0, 0xd8,
                              0xe0, 0xe8, 0xf0 };
    for (size_t off : offsets) {
        if (try_pointer_and_length(off, off + 8, off + 16)) {
            return true;
        }
    }

    // Try fallback: scan for any pointer that points to ASCII
    for (size_t off = 0; off + sizeof(uint64_t) <= header.size(); off += sizeof(uint64_t)) {
        uint64_t maybe_ptr = read_u64(off);
        if (read_remote_ascii(maybe_ptr, 2048, out)) {
            MH_LOG("[extract.fallback] found string via pointer at offset 0x%zx -> %p len=%zu",
                off, (void*)maybe_ptr, out.size());
            if (out_layout && !out.empty()) {
                // Try to infer layout from pointer
                if (off + 24 <= header.size()) {
                    uint64_t maybe_size = read_u64(off + 8);
                    uint64_t maybe_cap = read_u64(off + 16);
                    if (maybe_cap > 0 && maybe_cap <= (1ULL << 20) &&
                        (maybe_size == 0 || (maybe_size <= (1ULL << 20) && maybe_cap >= maybe_size))) {
                        out_layout->valid = true;
                        out_layout->data_off = off;
                        out_layout->size_off = off + 8;
                        out_layout->cap_off = off + 16;
                        out_layout->header_span = std::max<size_t>(0x40, off + 24);
                    }
                }
            }
            return true;
        }
    }

    return false;
}
