#include "update_checker.h"
#include "version.h"
#include "proxy.h"
#include "util.h"

#include <orbis/libkernel.h>
#include <orbis/Http.h>
#include <orbis/Ssl.h>
#include <orbis/Net.h>
#include <orbis/Sysmodule.h>
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

extern "C" void mh_log(const char* fmt, ...);

namespace {
void update_log(const char* fmt, ...);

constexpr const char* kReleaseApi = "https://api.github.com/repos/FourZeroCode/ps4_yt_adsponsorblock/releases/latest";
static pthread_t g_update_thread{};
static volatile bool g_update_started = false;
static volatile bool g_update_cancel = false;
static pthread_mutex_t g_install_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_install_started = false;
static std::string g_asset_url;


int verify_github_certificate(int, unsigned int verify_error, void* const[], int, void*) {
    return verify_error == 0 ? 1 : 0;
}

bool parse_version(const std::string& input, int out[3]) {
    const char* p = input.c_str();
    if (*p == 'v' || *p == 'V') ++p;
    char* end = nullptr;
    for (int i = 0; i < 3; ++i) {
        if (*p < '0' || *p > '9') return false;
        long n = strtol(p, &end, 10);
        if (end == p || n < 0 || n > 100000) return false;
        out[i] = (int)n;
        p = end;
        if (i != 2) { if (*p != '.') return false; ++p; }
    }
    return *p == '\0';
}

bool is_newer(const int a[3], const int b[3]) {
    for (int i = 0; i < 3; ++i) { if (a[i] != b[i]) return a[i] > b[i]; }
    return false;
}


bool extract_json_message(const std::string& json, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    const char key[] = "\"message\"";
    size_t p = json.find(key);
    if (p == std::string::npos) return false;
    p = json.find(':', p + sizeof(key) - 1);
    if (p == std::string::npos) return false;
    p = json.find('"', p + 1);
    if (p == std::string::npos) return false;
    ++p;
    size_t used = 0;
    bool escaped = false;
    for (; p < json.size(); ++p) {
        char c = json[p];
        if (escaped) {
            if (c == 'n' || c == 'r') c = ' ';
            if (used + 1 < cap) out[used++] = c;
            escaped = false;
            continue;
        }
        if (c == '\\') { escaped = true; continue; }
        if (c == '"') break;
        if ((unsigned char)c < 0x20) c = ' ';
        if (used + 1 < cap) out[used++] = c;
    }
    out[used] = '\0';
    return used > 0;
}

bool get_latest_release(std::string& body, const char*& failure) {
    int pool = -1, ssl = -1, http = -1, templ = -1, conn = -1, req = -1;
    bool ok = false;
    int status = 0;
    static char failure_detail[512];
    failure = "Unknown GitHub request failure";
    typedef int32_t (*SetHeaderMaxFn)(int32_t, uint32_t);
    auto set_header_max = reinterpret_cast<SetHeaderMaxFn>(&sceHttpSetResponseHeaderMaxSize);

    // Networking is already initialized by the plugin environment.
    // Avoid internal module IDs; they aren't available in every GoldHEN SDK.
    sceNetInit();
    pool = sceNetPoolCreate("YTAdBlockUpdate", 16 * 1024, 0);
    if (pool < 0) { failure = "sceNetPoolCreate failed"; goto done; }
    ssl = sceSslInit(512 * 1024);
    if (ssl < 0) { failure = "sceSslInit failed"; goto done; }
    http = sceHttpInit(pool, ssl, 512 * 1024);
    if (http < 0) { failure = "sceHttpInit failed"; goto done; }
    templ = sceHttpCreateTemplate(http, "YTAdBlock-Updater/1.0", ORBIS_HTTP_VERSION_1_1, 1);
    if (templ < 0) { failure = "HTTP template creation failed"; goto done; }
    if (sceHttpsSetSslCallback(templ, verify_github_certificate, nullptr) < 0) { failure = "HTTPS certificate callback setup failed"; goto done; }
    if (set_header_max(templ, 64 * 1024) < 0) { failure = "HTTP response header setup failed"; goto done; }
    conn = sceHttpCreateConnectionWithURL(templ, kReleaseApi, 1);
    if (conn < 0) { failure = "GitHub connection creation failed"; goto done; }
    req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, kReleaseApi, 0);
    if (req < 0) { failure = "GitHub request creation failed"; goto done; }
    if (sceHttpAddRequestHeader(req, "User-Agent", "YTAdBlock-SponsorBlock-PS4", 0) < 0 ||
        sceHttpAddRequestHeader(req, "Accept", "application/vnd.github+json", 0) < 0) { failure = "GitHub request headers failed"; goto done; }
    if (sceHttpSendRequest(req, nullptr, 0) < 0) { failure = "GitHub request send failed"; goto done; }
    if (sceHttpGetStatusCode(req, &status) < 0) { failure = "Could not read GitHub HTTP status"; goto done; }
    if (status != 200) {
        // Include GitHub's error message in the updater log.
        char response_buf[2048];
        std::string error_body;
        for (;;) {
            int n = sceHttpReadData(req, response_buf, sizeof(response_buf));
            if (n <= 0) break;
            if (error_body.size() + (size_t)n > 8192) break;
            error_body.append(response_buf, (size_t)n);
        }
        char github_message[320] = {0};
        if (extract_json_message(error_body, github_message, sizeof(github_message))) {
            snprintf(failure_detail, sizeof(failure_detail), "GitHub HTTP %d: %.400s", status, github_message);
        } else {
            const char* reason = status == 502 ? "Bad Gateway" :
                                 status == 503 ? "Service Unavailable" :
                                 status == 504 ? "Gateway Timeout" :
                                 status == 500 ? "Internal Server Error" :
                                 status == 429 ? "Rate Limited" :
                                 status == 403 ? "Forbidden" :
                                 status == 404 ? "Not Found" :
                                 "Unexpected HTTP response";
            snprintf(failure_detail, sizeof(failure_detail), "GitHub HTTP %d (%s)", status, reason);
        }
        mh_log("[updater] %s\n", failure_detail);
        failure = failure_detail;
        goto done;
    }
    for (;;) {
        char buf[2048];
        int n = sceHttpReadData(req, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { failure = "Reading GitHub response failed"; body.clear(); goto done; }
        if (body.size() + (size_t)n > 256 * 1024) { failure = "GitHub response exceeded size limit"; body.clear(); goto done; }
        body.append(buf, (size_t)n);
    }
    ok = !body.empty();
    if (!ok) failure = "GitHub returned an empty response";
done:
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (templ >= 0) sceHttpDeleteTemplate(templ);
    if (http >= 0) sceHttpTerm(http);
    if (ssl >= 0) sceSslTerm();
    if (pool >= 0) sceNetPoolDestroy(pool);
    return ok;
}

bool extract_tag(const std::string& json, std::string& tag) {
    const std::string key = "\"tag_name\"";
    size_t p = json.find(key);
    if (p == std::string::npos || (p = json.find(':', p + key.size())) == std::string::npos) return false;
    p = json.find('"', p + 1);
    if (p == std::string::npos) return false;
    size_t e = json.find('"', p + 1);
    if (e == std::string::npos) return false;
    tag.assign(json, p + 1, e - p - 1);
    return true;
}

bool extract_asset_url(const std::string& json, std::string& url) {
    const std::string name_key = "\"name\"";
    size_t search = 0;
    while ((search = json.find(name_key, search)) != std::string::npos) {
        size_t colon = json.find(':', search + name_key.size());
        if (colon == std::string::npos) return false;
        size_t value = json.find('"', colon + 1);
        if (value == std::string::npos) return false;
        size_t value_end = json.find('"', value + 1);
        if (value_end == std::string::npos) return false;
        if (json.compare(value + 1, value_end - value - 1, "ps4_yt_adsponsorblock.prx") != 0) {
            search = value_end + 1;
            continue;
        }
        size_t object_start = json.rfind('{', search);
        if (object_start == std::string::npos) return false;
        int depth = 0;
        bool in_string = false, escaped = false;
        size_t object_end = std::string::npos;
        for (size_t i = object_start; i < json.size(); ++i) {
            char c = json[i];
            if (in_string) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') in_string = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) { object_end = i; break; }
        }
        if (object_end == std::string::npos) return false;
        const std::string url_key = "\"browser_download_url\"";
        size_t key = json.find(url_key, value_end + 1);
        if (key == std::string::npos || key > object_end) return false;
        size_t p = json.find(':', key + url_key.size());
        if (p == std::string::npos || p > object_end) return false;
        p = json.find('"', p + 1);
        if (p == std::string::npos || p > object_end) return false;
        size_t e = json.find('"', p + 1);
        if (e == std::string::npos || e > object_end) return false;
        url.assign(json, p + 1, e - p - 1);
        return url.compare(0, 8, "https://") == 0;
    }
    return false;
}


const char* kDataDirectory = "/data/ytadsponsorblock";
const char* kDownloadPath = "/data/ytadsponsorblock/ps4_yt_adsponsorblock.prx.download";
const char* kPluginPath = "/data/GoldHEN/plugins/ps4_yt_adsponsorblock.prx";
const char* kBackupBasePath = "/data/ytadsponsorblock/ps4_yt_adsponsorblock.prx";
const char* kUpdateLogPath = "/data/ytadsponsorblock/update_log.txt";
const char* kStartupCheckTimestampPath = "/data/ytadsponsorblock/startup_check.timestamp";
const char* kStartupCheckTimestampTempPath = "/data/ytadsponsorblock/startup_check.timestamp.tmp";

void reset_update_log() {
    // Start a new log for each launch. update_log() appends to it.
    if (mkdir(kDataDirectory, 0777) != 0 && errno != EEXIST) return;
    int fd = open(kUpdateLogPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    close(fd);
}

void update_log(const char* fmt, ...) {
    // Keep updater files separate from the plugin directory.
    if (mkdir(kDataDirectory, 0777) != 0 && errno != EEXIST) return;
    int fd = open(kUpdateLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    char line[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line)-1;
        write(fd, line, len);
        write(fd, "\n", 1);
    }
    close(fd);
}


bool startup_check_interval_elapsed() {
    const time_t now = time(nullptr);
    if (now <= 0) {
        update_log("STARTUP CHECK TIMER: system time unavailable; allowing check");
        return true;
    }

    int fd = open(kStartupCheckTimestampPath, O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT)
            update_log("STARTUP CHECK TIMER: could not read timestamp (errno=%d); allowing check", errno);
        return true;
    }

    char buffer[64] = {0};
    ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (count <= 0) {
        update_log("STARTUP CHECK TIMER: timestamp file is empty/unreadable; allowing check");
        return true;
    }

    char* end = nullptr;
    long long last_check = strtoll(buffer, &end, 10);
    if (end == buffer || last_check <= 0) {
        update_log("STARTUP CHECK TIMER: timestamp file is invalid; allowing check");
        return true;
    }

    if (now < (time_t)last_check) {
        update_log("STARTUP CHECK TIMER: system clock moved backwards; allowing check");
        return true;
    }

    const long long elapsed = (long long)now - last_check;
    if (elapsed < 24LL * 60LL * 60LL) {
        const long long remaining = 24LL * 60LL * 60LL - elapsed;
        update_log("AUTO UPDATE CHECK SKIPPED: 24-hour interval has not elapsed (%lld seconds remaining)", remaining);
        return false;
    }
    return true;
}

void record_startup_check_time() {
    const time_t now = time(nullptr);
    if (now <= 0) {
        update_log("STARTUP CHECK TIMER: could not record check time because system time is unavailable");
        return;
    }

    if (mkdir(kDataDirectory, 0777) != 0 && errno != EEXIST) {
        update_log("STARTUP CHECK TIMER: could not create data directory (errno=%d)", errno);
        return;
    }

    char timestamp[32];
    int length = snprintf(timestamp, sizeof(timestamp), "%lld\n", (long long)now);
    if (length <= 0 || (size_t)length >= sizeof(timestamp)) {
        update_log("STARTUP CHECK TIMER: timestamp formatting failed");
        return;
    }

    int fd = open(kStartupCheckTimestampTempPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        update_log("STARTUP CHECK TIMER: could not create timestamp file (errno=%d)", errno);
        return;
    }

    ssize_t written = write(fd, timestamp, (size_t)length);
    bool ok = written == length && fsync(fd) == 0;
    int saved_errno = errno;
    if (close(fd) != 0 && ok) {
        ok = false;
        saved_errno = errno;
    }
    if (!ok || rename(kStartupCheckTimestampTempPath, kStartupCheckTimestampPath) != 0) {
        if (ok) saved_errno = errno;
        unlink(kStartupCheckTimestampTempPath);
        update_log("STARTUP CHECK TIMER: could not save timestamp (errno=%d)", saved_errno);
        return;
    }

    update_log("STARTUP CHECK TIMER: recorded automatic check time=%lld", (long long)now);
}

bool extract_redirect_location(int request, char* out, size_t cap) {
    char* headers = nullptr; size_t size = 0;
    if (sceHttpGetAllResponseHeaders(request, &headers, &size) < 0 || !headers || !size) return false;
    size_t pos = 0;
    while (pos < size) {
        size_t begin = pos;
        while (pos < size && headers[pos] != '\n') ++pos;
        size_t end = pos;
        if (pos < size) ++pos;
        if (end > begin && headers[end-1] == '\r') --end;
        static const char key[] = "location:";
        if (end - begin >= sizeof(key)-1) {
            bool match = true;
            for (size_t i=0; i<sizeof(key)-1; ++i) {
                char c=headers[begin+i]; if (c>='A' && c<='Z') c=(char)(c-'A'+'a');
                if (c != key[i]) { match=false; break; }
            }
            if (match) {
                size_t v=begin+sizeof(key)-1;
                while (v<end && (headers[v]==' ' || headers[v]=='\t')) ++v;
                size_t n=end-v;
                if (!n || n>=cap) return false;
                memcpy(out, headers+v, n); out[n]=0;
                return strncmp(out,"https://",8)==0;
            }
        }
    }
    return false;
}

bool validate_downloaded_prx(const char* path, std::string& failure, uint64_t* validated_bytes = nullptr) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Downloaded PRX cannot be opened (errno %d)", errno);
        failure = msg;
        update_log("VALIDATION FAILED: %s", msg);
        return false;
    }

    // stat() reported the wrong size on the PS4, while FTP showed the
    // correct size. Count the bytes as we read them instead.
    unsigned char header[4] = {0, 0, 0, 0};
    uint64_t total = 0;
    bool have_header = false;
    unsigned char buffer[16384];
    bool read_ok = true;
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            read_ok = false;
            break;
        }
        if (!have_header) {
            if (n < 4) {
                memcpy(header, buffer, (size_t)n);
                // A short read doesn't necessarily mean we're at EOF.
                if (n > 0) total += (uint64_t)n;
                continue;
            }
            memcpy(header, buffer, sizeof(header));
            have_header = true;
        }
        total += (uint64_t)n;
        if (total > 20ULL * 1024ULL * 1024ULL) {
            read_ok = false;
            break;
        }
    }
    int close_result = close(fd);
    if (!read_ok || close_result != 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Downloaded PRX read/close failed (errno %d)", errno);
        failure = msg;
        update_log("VALIDATION FAILED: %s", msg);
        return false;
    }
    if (total < 65536) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Downloaded PRX is too small (%llu bytes)", (unsigned long long)total);
        failure = msg;
        update_log("VALIDATION FAILED: %s", msg);
        return false;
    }
    static const unsigned char expected[4] = {0x4F, 0x15, 0x3D, 0x1D};
    if (!have_header || memcmp(header, expected, sizeof(expected)) != 0) {
        failure = "Downloaded file has an unexpected PS4 SELF/PRX header";
        update_log("VALIDATION FAILED: invalid header %02X %02X %02X %02X",
                   header[0], header[1], header[2], header[3]);
        return false;
    }
    update_log("VALIDATION OK path=%s bytes_read=%llu header=4F153D1D",
               path, (unsigned long long)total);
    if (validated_bytes) *validated_bytes = total;
    return true;
}

bool write_all_fd(int fd, const unsigned char* data, size_t length) {
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, data + done, length - done);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        done += (size_t)n;
    }
    return true;
}

bool copy_fd_contents(int source_fd, int destination_fd, uint64_t& copied) {
    unsigned char buffer[16384];
    copied = 0;
    for (;;) {
        ssize_t n = read(source_fd, buffer, sizeof(buffer));
        if (n == 0) return true;
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (!write_all_fd(destination_fd, buffer, (size_t)n)) return false;
        copied += (uint64_t)n;
    }
}

bool create_backup_copy(char* out, size_t cap, uint64_t& backup_bytes) {
    int source_fd = open(kPluginPath, O_RDONLY);
    if (source_fd < 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Cannot open current PRX for backup (errno %d)", errno);
        update_log("BACKUP FAILED: %s", msg);
        return false;
    }

    char temp_path[512];
    snprintf(out, cap, "%s.bak", kBackupBasePath);
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", out);

    // Finish the temp copy before replacing the existing backup.
    int backup_fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (backup_fd < 0) {
        int e = errno; close(source_fd);
        update_log("BACKUP CREATE FAILED path=%s errno=%d", temp_path, e);
        return false;
    }

    (void)fchmod(backup_fd, 0777);
    bool ok = copy_fd_contents(source_fd, backup_fd, backup_bytes);
    int saved = errno;
    if (ok && fsync(backup_fd) != 0) { ok = false; saved = errno; }
    if (close(source_fd) != 0 && ok) { ok = false; saved = errno; }
    if (close(backup_fd) != 0 && ok) { ok = false; saved = errno; }
    if (!ok) {
        unlink(temp_path);
        update_log("BACKUP COPY FAILED errno=%d", saved);
        return false;
    }

    if (rename(temp_path, out) != 0) {
        int e = errno;
        unlink(temp_path);
        update_log("BACKUP REPLACE FAILED path=%s errno=%d", out, e);
        return false;
    }

    // Clean up numbered backups from older versions.
    for (int i = 1; i < 100; ++i) {
        char old_backup[512];
        snprintf(old_backup, sizeof(old_backup), "%s.bak.%d", kBackupBasePath, i);
        unlink(old_backup);
    }

    update_log("BACKUP CREATED path=%s bytes=%llu", out, (unsigned long long)backup_bytes);
    return true;
}

bool copy_download_into_active_prx(uint64_t& installed_bytes, std::string& failure) {
    int source_fd = open(kDownloadPath, O_RDONLY);
    if (source_fd < 0) {
        char msg[160]; snprintf(msg, sizeof(msg), "Cannot open downloaded PRX (errno %d)", errno);
        failure = msg; update_log("INSTALL FAILED: %s", msg); return false;
    }

    // Use the same install sequence as my pkg installer.
    int destination_fd = open(kPluginPath, O_WRONLY | O_CREAT, 0777);
    if (destination_fd < 0) {
        int e = errno; close(source_fd);
        char msg[160]; snprintf(msg, sizeof(msg), "Cannot open active PRX for writing (errno %d)", e);
        failure = msg; update_log("INSTALL FAILED: %s", msg); return false;
    }

    if (fchmod(destination_fd, 0777) != 0) {
        int chmod_errno = errno;
        struct stat st;
        if (chmod_errno != EPERM || fstat(destination_fd, &st) != 0 || (st.st_mode & 0777) != 0777) {
            close(source_fd); close(destination_fd);
            char msg[160]; snprintf(msg, sizeof(msg), "Cannot set PRX permissions to 0777 (errno %d)", chmod_errno);
            failure = msg; update_log("INSTALL FAILED: %s", msg); return false;
        }
        update_log("FCHMOD returned EPERM but existing mode is already 0777");
    }
    struct stat mode_info;
    if (fstat(destination_fd, &mode_info) == 0)
        update_log("DESTINATION MODE BEFORE TRUNCATE=%04o", (unsigned)(mode_info.st_mode & 0777));

    if (ftruncate(destination_fd, 0) != 0) {
        int e = errno; close(source_fd); close(destination_fd);
        char msg[160]; snprintf(msg, sizeof(msg), "Cannot truncate active PRX (errno %d)", e);
        failure = msg; update_log("INSTALL FAILED: %s", msg); return false;
    }

    bool ok = copy_fd_contents(source_fd, destination_fd, installed_bytes);
    int saved = errno;
    if (ok && fsync(destination_fd) != 0) { ok = false; saved = errno; }
    if (close(source_fd) != 0 && ok) { ok = false; saved = errno; }
    if (close(destination_fd) != 0 && ok) { ok = false; saved = errno; }
    if (!ok) {
        char msg[160]; snprintf(msg, sizeof(msg), "Writing active PRX failed (errno %d)", saved);
        failure = msg; update_log("INSTALL FAILED: %s", msg); return false;
    }
    update_log("ACTIVE PRX WRITE COMPLETE bytes=%llu mode=0777", (unsigned long long)installed_bytes);
    return true;
}

bool install_downloaded_prx(std::string& failure) {
    update_log("INSTALL START download=%s target=%s", kDownloadPath, kPluginPath);
    uint64_t expected_bytes = 0;
    if (!validate_downloaded_prx(kDownloadPath, failure, &expected_bytes)) return false;

    // lstat() returned EPERM here even though the file was visible over FTP.
    // Open the plugin and make a backup before replacing it.
    char backup_path[256] = {0};
    uint64_t backup_bytes = 0;
    if (!create_backup_copy(backup_path, sizeof(backup_path), backup_bytes)) {
        failure = "Could not create a safe backup of the current PRX";
        return false;
    }

    if (backup_bytes < 65536) {
        failure = "Existing PRX backup is unexpectedly small; active file was not changed";
        update_log("INSTALL ABORTED: backup too small bytes=%llu", (unsigned long long)backup_bytes);
        return false;
    }

    uint64_t installed_bytes = 0;
    if (!copy_download_into_active_prx(installed_bytes, failure)) {
        update_log("INSTALL WRITE FAILED; attempting rollback from %s", backup_path);
        int backup_fd = open(backup_path, O_RDONLY);
        int target_fd = open(kPluginPath, O_WRONLY | O_CREAT, 0777);
        bool rollback_ok = backup_fd >= 0 && target_fd >= 0;
        if (rollback_ok) {
            (void)fchmod(target_fd, 0777);
            rollback_ok = ftruncate(target_fd, 0) == 0;
            uint64_t restored = 0;
            if (rollback_ok) rollback_ok = copy_fd_contents(backup_fd, target_fd, restored) && restored == backup_bytes;
            if (rollback_ok) rollback_ok = fsync(target_fd) == 0;
        }
        if (backup_fd >= 0) close(backup_fd);
        if (target_fd >= 0) close(target_fd);
        update_log("ROLLBACK %s backup=%s", rollback_ok ? "SUCCESS" : "FAILED", backup_path);
        return false;
    }

    if (installed_bytes != expected_bytes) {
        char msg[192];
        snprintf(msg, sizeof(msg), "Installed byte count mismatch (expected %llu, wrote %llu)",
                 (unsigned long long)expected_bytes, (unsigned long long)installed_bytes);
        failure = msg;
        update_log("INSTALL FAILED: %s; attempting rollback", msg);
        int backup_fd = open(backup_path, O_RDONLY);
        int target_fd = open(kPluginPath, O_WRONLY | O_CREAT, 0777);
        bool rollback_ok = backup_fd >= 0 && target_fd >= 0;
        if (rollback_ok) {
            (void)fchmod(target_fd, 0777);
            rollback_ok = ftruncate(target_fd, 0) == 0;
            uint64_t restored = 0;
            if (rollback_ok) rollback_ok = copy_fd_contents(backup_fd, target_fd, restored) && restored == backup_bytes;
            if (rollback_ok) rollback_ok = fsync(target_fd) == 0;
        }
        if (backup_fd >= 0) close(backup_fd);
        if (target_fd >= 0) close(target_fd);
        update_log("ROLLBACK %s backup=%s", rollback_ok ? "SUCCESS" : "FAILED", backup_path);
        return false;
    }
    update_log("INSTALL SUCCESS path=%s bytes=%llu mode=0777", kPluginPath, (unsigned long long)installed_bytes);
    update_log("PREVIOUS VERSION PRESERVED path=%s bytes=%llu", backup_path, (unsigned long long)backup_bytes);
    // Remove the download only after the install is verified. Keep the backup
    // in case we need to rollback.
    if (unlink(kDownloadPath) == 0) update_log("DOWNLOAD TEMP REMOVED");
    else update_log("WARNING: could not remove download temp errno=%d", errno);
    failure = "Update installed successfully";
    return true;
}

bool download_asset(const std::string& url, const char* output_path, std::string& failure) {
    int pool=-1, ssl=-1, http=-1, templ=-1, conn=-1, req=-1, fd=-1;
    int status=0, content_type=0;
    size_t content_length=0;
    uint64_t total=0;
    bool ok=false, final_response=false;
    char current_url[2048], buffer[16384];
    typedef int32_t (*SetHeaderMaxFn)(int32_t,uint32_t);
    SetHeaderMaxFn set_header_max=reinterpret_cast<SetHeaderMaxFn>(&sceHttpSetResponseHeaderMaxSize);
    snprintf(current_url,sizeof(current_url),"%s",url.c_str());
    update_log("START download url=%s", current_url);

    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET) < 0) {
        failure="NET module load failed"; update_log("%s",failure.c_str()); goto done;
    }
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP) < 0) {
        failure="HTTP module load failed"; update_log("%s",failure.c_str()); goto done;
    }
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL) < 0) {
        failure="SSL module load failed"; update_log("%s",failure.c_str()); goto done;
    }
    sceNetInit();
    pool=sceNetPoolCreate("YTAdBlockDl",64*1024,0);
    if(pool<0){failure="Network pool creation failed"; update_log("%s (%d)",failure.c_str(),pool);goto done;}
    ssl=sceSslInit(512*1024);
    if(ssl<0){failure="SSL initialization failed";update_log("%s (%d)",failure.c_str(),ssl);goto done;}
    http=sceHttpInit(pool,ssl,512*1024);
    if(http<0){failure="HTTP initialization failed";update_log("%s (%d)",failure.c_str(),http);goto done;}
    templ=sceHttpCreateTemplate(http,"YTAdBlock-Updater/1.0",ORBIS_HTTP_VERSION_1_1,1);
    if(templ<0){failure="HTTP template creation failed";update_log("%s (%d)",failure.c_str(),templ);goto done;}
    if(sceHttpsSetSslCallback(templ,verify_github_certificate,nullptr)<0){failure="TLS certificate callback setup failed";update_log("%s",failure.c_str());goto done;}
    if(set_header_max(templ,64*1024)<0){failure="HTTP response header limit setup failed";update_log("%s",failure.c_str());goto done;}

    for(int redirects=0; redirects<=5; ++redirects) {
        conn=sceHttpCreateConnectionWithURL(templ,current_url,1);
        if(conn<0){failure="HTTP connection creation failed";update_log("%s (%d)",failure.c_str(),conn);goto done;}
        req=sceHttpCreateRequestWithURL(conn,ORBIS_METHOD_GET,current_url,0);
        if(req<0){failure="HTTP request creation failed";update_log("%s (%d)",failure.c_str(),req);goto done;}
        sceHttpAddRequestHeader(req,"User-Agent","YTAdBlock-SponsorBlock-PS4",0);
        sceHttpAddRequestHeader(req,"Accept","application/octet-stream",0);
        int rc=sceHttpSendRequest(req,nullptr,0);
        if(rc<0){int e=0;sceHttpGetLastErrno(req,&e);failure="HTTP send failed";update_log("%s rc=%d errno=%d",failure.c_str(),rc,e);goto done;}
        if(sceHttpGetStatusCode(req,&status)<0){failure="Could not read HTTP status";update_log("%s",failure.c_str());goto done;}
        update_log("HTTP response status=%d redirect=%d",status,redirects);
        if(status==200){final_response=true;break;}
        if(status==301||status==302||status==303||status==307||status==308) {
            char next[2048];
            if(redirects==5 || !extract_redirect_location(req,next,sizeof(next))) {
                failure="Redirect missing, unsafe, or limit exceeded";update_log("%s",failure.c_str());goto done;
            }
            update_log("REDIRECT to %s",next);
            sceHttpDeleteRequest(req);req=-1;
            sceHttpDeleteConnection(conn);conn=-1;
            snprintf(current_url,sizeof(current_url),"%s",next);
            continue;
        }
        char msg[128];snprintf(msg,sizeof(msg),"GitHub asset returned HTTP %d",status);failure=msg;update_log("%s",msg);goto done;
    }
    if(!final_response){failure="Redirect limit exceeded";update_log("%s",failure.c_str());goto done;}
    if(sceHttpGetResponseContentLength(req,&content_type,&content_length)==0)
        update_log("CONTENT_LENGTH=%lu",(unsigned long)content_length);

    fd=open(output_path,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd<0){char msg[128];snprintf(msg,sizeof(msg),"Cannot create download file (errno %d)",errno);failure=msg;update_log("%s",msg);goto done;}
    update_log("FILE_OPEN path=%s",output_path);
    for(;;){
        int n=sceHttpReadData(req,buffer,sizeof(buffer));
        if(n==0)break;
        if(n<0){char msg[128];snprintf(msg,sizeof(msg),"HTTP read failed (%d)",n);failure=msg;update_log("%s after %lu bytes",msg,(unsigned long)total);goto done;}
        if(total+(uint64_t)n>20ULL*1024*1024){failure="Download exceeded 20 MB limit";update_log("%s",failure.c_str());goto done;}
        size_t off=0;
        while(off<(size_t)n){ssize_t w=write(fd,buffer+off,(size_t)n-off);if(w<=0){char msg[128];snprintf(msg,sizeof(msg),"File write failed (errno %d)",errno);failure=msg;update_log("%s",msg);goto done;}off+=(size_t)w;}
        total+=(uint64_t)n;
    }
    update_log("RECEIVED_BYTES=%lu",(unsigned long)total);
    if(total<65536 || (content_length>0 && total!=content_length)){
        char msg[160];snprintf(msg,sizeof(msg),"Size validation failed: got %lu bytes, expected %lu",(unsigned long)total,(unsigned long)content_length);failure=msg;update_log("%s",msg);goto done;
    }
    if(fsync(fd)!=0){char msg[128];snprintf(msg,sizeof(msg),"fsync failed (errno %d)",errno);failure=msg;update_log("%s",msg);goto done;}
    if(close(fd)!=0){fd=-1;char msg[128];snprintf(msg,sizeof(msg),"close failed (errno %d)",errno);failure=msg;update_log("%s",msg);goto done;}
    fd=-1;
    ok=true; failure="Download saved successfully";
    update_log("SUCCESS bytes=%lu path=%s",(unsigned long)total,output_path);
done:
    if(fd>=0)close(fd);
    if(req>=0)sceHttpDeleteRequest(req);
    if(conn>=0)sceHttpDeleteConnection(conn);
    if(templ>=0)sceHttpDeleteTemplate(templ);
    if(http>=0)sceHttpTerm(http);
    if(ssl>=0)sceSslTerm();
    if(pool>=0)sceNetPoolDestroy(pool);
    if(!ok) {
        unlink(output_path);
        update_log("FAILED; partial file removed");
    }
    return ok;
}

void* download_worker(void*) {
    set_update_status("{\"state\":\"downloading\"}");
    update_log("UPDATE REQUEST received");
    std::string failure;
    if(g_asset_url.empty()) failure="Release asset URL is empty";
    else download_asset(g_asset_url,kDownloadPath,failure);
    if (failure == "Download saved successfully") {
        set_update_status("{\"state\":\"installing\"}");
        update_log("DOWNLOAD VERIFIED; beginning installation stage");
        if (install_downloaded_prx(failure)) {
            set_update_status("{\"state\":\"installed\"}");
        } else {
            char status[512];
            snprintf(status, sizeof(status), "{\"state\":\"install_error\",\"message\":\"%s\"}", failure.c_str());
            set_update_status(status);
            update_log("FINAL INSTALL ERROR: %s", failure.c_str());
        }
    } else {
        char status[512];
        snprintf(status, sizeof(status), "{\"state\":\"install_error\",\"message\":\"%s\"}", failure.c_str());
        set_update_status(status);
        update_log("FINAL DOWNLOAD ERROR: %s", failure.c_str());
    }
    pthread_mutex_lock(&g_install_mutex);
    g_install_started=false;
    pthread_mutex_unlock(&g_install_mutex);
    return nullptr;
}

void* update_worker(void* manual_request) {
    // Keep the toast delay for startup checks; manual checks start right away.
    if (!manual_request) {
        sceKernelUsleep(2500 * 1000);
        if (g_update_cancel) return nullptr;
        // Only automatic checks use the 24-hour cooldown.
		// Manual check bypasses the cooldown and always checks GitHub.
        record_startup_check_time();
    }
    if (g_update_cancel) return nullptr;
    set_update_status("{\"state\":\"checking\"}");
    mh_log("[updater] Checking GitHub release after welcome toast...\n");
    std::string json, latest;
    const char* failure = nullptr;
    if (!get_latest_release(json, failure)) {
        mh_log("[updater] GitHub request failed: %s\n", failure ? failure : "unknown error");
        update_log("AUTO UPDATE CHECK FAILED: %s", failure ? failure : "unknown error");
        char error_json[768];
        char escaped_message[640];
        const char* source_message = failure ? failure : "GitHub request failed";
        size_t out = 0;
        for (size_t i = 0; source_message[i] && out + 2 < sizeof(escaped_message); ++i) {
            unsigned char c = (unsigned char)source_message[i];
            if (c == '"' || c == '\\') escaped_message[out++] = '\\';
            if (c < 0x20) { escaped_message[out++] = ' '; continue; }
            escaped_message[out++] = (char)c;
        }
        escaped_message[out] = '\0';
        snprintf(error_json, sizeof(error_json), "{\"state\":\"error\",\"message\":\"%s\"}", escaped_message);
        set_update_status(error_json);
        return nullptr;
    }
    if (!extract_tag(json, latest)) {
        mh_log("[updater] GitHub responded, but tag_name could not be parsed\n");
        update_log("AUTO UPDATE CHECK FAILED: GitHub response received but tag_name was missing or invalid");
        set_update_status("{\"state\":\"error\",\"message\":\"GitHub response received but tag_name was missing or invalid\"}");
        return nullptr;
    }
    int current_parts[3], latest_parts[3];
    if (!parse_version(PS4_YT_ADSPONSORBLOCK_VERSION, current_parts) || !parse_version(latest, latest_parts)) {
        mh_log("[updater] Could not parse release version '%s'\n", latest.c_str());
        update_log("AUTO UPDATE CHECK FAILED: Invalid release version '%s' (installed version '%s')", latest.c_str(), PS4_YT_ADSPONSORBLOCK_VERSION);
        set_update_status("{\"state\":\"error\",\"message\":\"Release version format is invalid\"}");
        return nullptr;
    }
    if (is_newer(latest_parts, current_parts)) {
        char status[256];
        snprintf(status, sizeof(status), "{\"state\":\"available\",\"version\":\"%s\"}", latest.c_str());
        set_update_status(status);
        if (!extract_asset_url(json, g_asset_url)) {
            mh_log("[updater] Release found but ps4_yt_adsponsorblock.prx asset URL was not found\n");
            update_log("AUTO UPDATE CHECK FAILED: Release %s found, but ps4_yt_adsponsorblock.prx download URL was not found", latest.c_str());
            set_update_status("{\"state\":\"error\",\"message\":\"Release found, but ps4_yt_adsponsorblock.prx download URL was not found\"}");
            return nullptr;
        }
        mh_log("[updater] Update available: %s (installed: %s). Download/install is ready.\n", latest.c_str(), PS4_YT_ADSPONSORBLOCK_VERSION);
        update_log("AUTO UPDATE CHECK: update available latest=%s installed=%s", latest.c_str(), PS4_YT_ADSPONSORBLOCK_VERSION);
    } else {
        set_update_status("{\"state\":\"current\"}");
        mh_log("[updater] PRX is up to date (%s).\n", PS4_YT_ADSPONSORBLOCK_VERSION);
        update_log("AUTO UPDATE CHECK: up to date version=%s", PS4_YT_ADSPONSORBLOCK_VERSION);
    }
    return nullptr;
}
}

extern "C" void start_update_check_after_welcome_toast(void) {
    if (!is_update_check_on_startup_enabled()) {
        mh_log("[updater] Startup update check disabled in config.json\n");
        return;
    }
    if (g_update_started) return;
    g_update_cancel = false;
    reset_update_log();
    if (!startup_check_interval_elapsed()) {
        // Skipping a check due to the cooldown is normal.
        set_update_status("{\"state\":\"current\"}");
        return;
    }
    g_update_started = true;
    set_update_status("{\"state\":\"waiting\"}");
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int rc = pthread_create(&g_update_thread, &attr, update_worker, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        g_update_started = false;
        mh_log("[updater] Could not create update thread (%d)\n", rc);
        update_log("AUTO UPDATE CHECK FAILED: could not create update thread (error %d)", rc);
    }
}

extern "C" bool request_manual_update_check(void) {
    g_update_cancel = false;
    set_update_status("{\"state\":\"waiting\"}");
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int rc = pthread_create(&thread, &attr, update_worker, (void*)1);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        mh_log("[updater] Could not create manual update thread (%d)\n", rc);
        update_log("MANUAL UPDATE CHECK FAILED: could not create update thread (error %d)", rc);
        return false;
    }
    pthread_detach(thread);
    return true;
}

extern "C" bool request_update_install(void) {
    pthread_mutex_lock(&g_install_mutex);
    if (g_install_started || g_asset_url.empty()) { pthread_mutex_unlock(&g_install_mutex); return false; }
    g_install_started = true;
    pthread_mutex_unlock(&g_install_mutex);
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int rc = pthread_create(&thread, &attr, download_worker, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_lock(&g_install_mutex); g_install_started = false; pthread_mutex_unlock(&g_install_mutex);
        set_update_status("{\"state\":\"install_error\"}");
        return false;
    }
    pthread_detach(thread);
    return true;
}

extern "C" void stop_update_checker(void) {
    if (!g_update_started) return;
    g_update_cancel = true;
    pthread_join(g_update_thread, nullptr);
    g_update_started = false;
}
