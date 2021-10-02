/*MT*
    
    MediaTomb - http://www.mediatomb.cc/
    
    tools.cc - this file is part of MediaTomb.
    
    Copyright (C) 2005 Gena Batyan <bgeradz@mediatomb.cc>,
                       Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>
    
    Copyright (C) 2006-2010 Gena Batyan <bgeradz@mediatomb.cc>,
                            Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>,
                            Leonhard Wimmer <leo@mediatomb.cc>
    
    MediaTomb is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation.
    
    MediaTomb is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    version 2 along with MediaTomb; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
    
    $Id$
*/

/// \file tools.cc

#include "tools.h" // API

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <netdb.h>
#include <netinet/in.h>
#include <numeric>
#include <queue>
#include <random>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __HAIKU__
#define _DEFAULT_SOURCE
#endif

#include <ifaddrs.h>
#include <net/if.h>

#ifdef BSD_NATIVE_UUID
#include <uuid.h>
#else
#include <uuid/uuid.h>
#endif

#ifdef SOLARIS
#include <fcntl.h>
#include <sys/sockio.h>
#endif

#include "cds_objects.h"
#include "config/config_manager.h"
#include "contrib/md5.h"
#include "iohandler/file_io_handler.h"
#include "metadata/metadata_handler.h"

#define WHITE_SPACE " \t\r\n"

static constexpr auto hexChars = "0123456789abcdef";

std::vector<std::string> splitString(std::string_view str, char sep, bool empty)
{
    std::vector<std::string> ret;

    std::size_t pos = 0;
    while (pos < str.size()) {
        if (str[pos] == sep) {
            if (pos > 0 || empty)
                ret.emplace_back(str.substr(0, pos));
            str = str.substr(pos + 1);
            pos = 0;
        } else {
            ++pos;
        }
    }

    if (pos > 0 || empty)
        ret.emplace_back(str);

    return ret;
}

void trimStringInPlace(std::string& str)
{
    str.erase(str.begin(), std::find_if_not(str.begin(), str.end(), ::isspace));
    str.erase(std::find_if_not(str.rbegin(), str.rend(), ::isspace).base(), str.end());
}

std::string trimString(std::string str)
{
    if (str.empty())
        return str;

    trimStringInPlace(str);
    return str;
}

bool startswith(std::string_view str, std::string_view check)
{
#if __cpp_lib_starts_ends_with
    return str.starts_with(check);
#else
    return str.rfind(check, 0) == 0;
#endif
}

std::string& toLowerInPlace(std::string& str)
{
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
    return str;
}

std::string toLower(std::string str)
{
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
    return str;
}

int stoiString(const std::string& str, int def, int base)
{
    if (str.empty() || (str[0] == '-' && !std::isdigit(*str.substr(1).c_str())) || (str[0] != '-' && !std::isdigit(*str.c_str())))
        return def;

    return std::stoi(str, nullptr, base);
}

unsigned long stoulString(const std::string& str, int def, int base)
{
    if (str.empty() || (str[0] == '-' && !std::isdigit(*str.substr(1).c_str())) || (str[0] != '-' && !std::isdigit(*str.c_str())))
        return def;

    return std::stoul(str, nullptr, base);
}

void reduceString(std::string& str, char ch)
{
    auto newEnd = std::unique(str.begin(), str.end(),
        [=](char lhs, char rhs) { return (lhs == rhs) && (lhs == ch); });

    str.erase(newEnd, str.end());
}

void replaceString(std::string& str, std::string_view from, std::string_view to)
{
    auto startPos = str.find(from);
    if (startPos != std::string::npos)
        str.replace(startPos, from.length(), to);
}

void replaceAllString(std::string& str, std::string_view from, std::string_view to)
{
    auto startPos = str.find(from);
    while (startPos != std::string::npos) {
        str.replace(startPos, from.length(), to);
        startPos = str.find(from, startPos + to.length());
    }
}

bool isRegularFile(const fs::path& path, std::error_code& ec) noexcept
{
    // unfortunately fs::is_regular_file(path, ec) is broken with old libstdc++ on 32bit systems (see #737)
#if defined(__GLIBCXX__) && (__GLIBCXX__ <= 20190406)
    struct stat statbuf;
    int ret = stat(path.c_str(), &statbuf);
    if (ret != 0) {
        ec = std::make_error_code(std::errc(errno));
        return false;
    }

    ec.clear();
    return S_ISREG(statbuf.st_mode);
#else
    return fs::is_regular_file(path, ec);
#endif
}

bool isRegularFile(const fs::directory_entry& dirEnt, std::error_code& ec) noexcept
{
    // unfortunately fs::is_regular_file(path, ec) is broken with old libstdc++ on 32bit systems (see #737)
#if defined(__GLIBCXX__) && (__GLIBCXX__ <= 20190406)
    struct stat statbuf;
    int ret = stat(dirEnt.path().c_str(), &statbuf);
    if (ret != 0) {
        ec = std::make_error_code(std::errc(errno));
        return false;
    }

    ec.clear();
    return S_ISREG(statbuf.st_mode);
#else
    return dirEnt.is_regular_file(ec);
#endif
}

off_t getFileSize(const fs::directory_entry& dirEnt)
{
    // unfortunately fs::file_size() is broken with old libstdc++ on 32bit systems (see #737)
#if defined(__GLIBCXX__) && (__GLIBCXX__ <= 20190406)
    struct stat statbuf;
    int ret = stat(dirEnt.path().c_str(), &statbuf);
    if (ret != 0) {
        throw_std_runtime_error("{}: {}", std::strerror(errno), dirEnt.path().c_str());
    }

    return statbuf.st_size;
#else
    return dirEnt.file_size();
#endif
}

bool isExecutable(const fs::path& path, int* err)
{
    int ret = access(path.c_str(), R_OK | X_OK);
    if (err)
        *err = errno;

    return ret == 0;
}

fs::path findInPath(const fs::path& exec)
{
    auto p = getenv("PATH");
    if (!p)
        return {};

    std::string envPath = p;
    std::error_code ec;
    auto pathAr = splitString(envPath, ':');
    for (auto&& path : pathAr) {
        fs::path check = fs::path(path) / exec;
        if (isRegularFile(check, ec))
            return check;
    }

    return {};
}

std::string renderWebUri(std::string_view ip, int port)
{
    return fmt::format((ip.find(':') != std::string::npos) ? "[{}]:{}" : "{}:{}", ip, port);
}

std::string httpRedirectTo(std::string_view addr, std::string_view page)
{
    return fmt::format(R"(<html><head><meta http-equiv="Refresh" content="0;URL={}/{}"></head><body bgcolor="#dddddd"></body></html>)", addr, page);
}

std::string hexEncode(const void* data, std::size_t len)
{
    std::string buf(2 * len, '\0');

    const unsigned char* chars = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; i++) {
        unsigned char c = chars[i];
        unsigned char hi = c >> 4;
        unsigned char lo = c & 0xF;
        buf[2 * i + 0] = hexChars[hi];
        buf[2 * i + 1] = hexChars[lo];
    }

    return buf;
}

std::string hexDecodeString(std::string_view encoded)
{
    const char* ptr = encoded.data();
    std::size_t len = encoded.length();

    std::string buf(len / 2, '\0');
    for (std::size_t i = 0; i < len; i += 2) {
        auto chi = std::strchr(hexChars, ptr[i]);
        auto clo = std::strchr(hexChars, ptr[i + 1]);
        std::size_t hi = chi ? chi - hexChars : 0;
        std::size_t lo = clo ? clo - hexChars : 0;

        auto ch = static_cast<char>(hi << 4 | lo);
        buf[i / 2] = ch;
    }
    return buf;
}

std::string hexMd5(const void* data, std::size_t length)
{
    char md5buf[16];

    md5_state_t ctx;
    md5_init(&ctx);
    md5_append(&ctx, static_cast<unsigned char*>(const_cast<void*>(data)), length);
    md5_finish(&ctx, reinterpret_cast<unsigned char*>(md5buf));

    return hexEncode(md5buf, 16);
}

std::string hexStringMd5(std::string_view str)
{
    return hexMd5(str.data(), str.length());
}

std::string generateRandomId()
{
#ifdef BSD_NATIVE_UUID
    char* uuidStr;
    std::uint32_t status;
#else
    char uuidStr[37];
#endif
    uuid_t uuid;

#ifdef BSD_NATIVE_UUID
    uuid_create(&uuid, &status);
    uuid_to_string(&uuid, &uuidStr, &status);
#else
    uuid_generate(uuid);
    uuid_unparse(uuid, uuidStr);
#endif

    log_debug("Generated: {}", uuidStr);
    auto uuidString = std::string(uuidStr);
#ifdef BSD_NATIVE_UUID
    free(uuidStr);
#endif

    return uuidString;
}

static constexpr auto hexCharS2 = "0123456789ABCDEF";

std::string urlEscape(std::string_view str)
{
    std::ostringstream buf;
    for (std::size_t i = 0; i < str.length();) {
        auto c = str[i];
        int cplen = 1;
        if ((c & 0xf8) == 0xf0)
            cplen = 4;
        else if ((c & 0xf0) == 0xe0)
            cplen = 3;
        else if ((c & 0xe0) == 0xc0)
            cplen = 2;
        if ((i + cplen) > str.length())
            cplen = 1;

        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-') {
            buf << char(c);
        } else {
            int hi = c >> 4;
            int lo = c & 15;
            if (cplen > 1)
                buf << str.substr(i, cplen);
            else
                buf << '%' << hexCharS2[hi] << hexCharS2[lo];
        }
        i += cplen;
    }
    return buf.str();
}

std::string urlUnescape(std::string_view str)
{
    auto data = str.data();
    int len = str.length();
    std::ostringstream buf;

    int i = 0;
    while (i < len) {
        const char c = data[i++];
        if (c == '%') {
            if (i + 2 > len)
                break; // avoid buffer overrun
            char chi = data[i++];
            char clo = data[i++];
            int hi, lo;

            const char* pos;

            pos = std::strchr(hexCharS2, chi);
            hi = pos ? pos - hexCharS2 : 0;

            pos = std::strchr(hexCharS2, clo);
            lo = pos ? pos - hexCharS2 : 0;

            int ascii = (hi << 4) | lo;
            buf << char(ascii);
        } else if (c == '+') {
            buf << ' ';
        } else {
            buf << c;
        }
    }
    return buf.str();
}

static std::string dictEncode(const std::map<std::string, std::string>& dict, char sep1, char sep2)
{
    std::ostringstream buf;
    for (auto it = dict.begin(); it != dict.end(); ++it) {
        if (it != dict.begin())
            buf << sep1;
        buf << urlEscape(it->first) << sep2
            << urlEscape(it->second);
    }
    return buf.str();
}

std::string dictEncode(const std::map<std::string, std::string>& dict)
{
    return dictEncode(dict, '&', '=');
}

std::string dictEncodeSimple(const std::map<std::string, std::string>& dict)
{
    return dictEncode(dict, '/', '/');
}

std::map<std::string, std::string> dictDecode(std::string_view url, bool unEscape)
{
    std::map<std::string, std::string> dict;
    const char* data = url.data();
    const char* dataEnd = data + url.length();
    while (data < dataEnd) {
        const char* ampPos = std::strchr(data, '&');
        if (!ampPos) {
            ampPos = dataEnd;
        }
        const char* eqPos = std::strchr(data, '=');
        if (eqPos && eqPos < ampPos) {
            std::string_view key(data, eqPos - data);
            std::string_view value(eqPos + 1, ampPos - eqPos - 1);
            if (unEscape) {
                dict.emplace(urlUnescape(key), urlUnescape(value));
            } else {
                dict.emplace(key, value);
            }
        }
        data = ampPos + 1;
    }
    return dict;
}

// this is somewhat tricky as we need an exact amount of pairs
// object_id=720&res_id=0
std::map<std::string, std::string> dictDecodeSimple(std::string_view url)
{
    std::map<std::string, std::string> dict;
    std::size_t pos;
    std::size_t lastPos = 0;
    do {
        pos = url.find('/', lastPos);
        if (pos == std::string_view::npos || pos < lastPos + 1)
            break;

        std::string key = urlUnescape(url.substr(lastPos, pos - lastPos));
        lastPos = pos + 1;
        pos = url.find('/', lastPos);
        if (pos == std::string::npos)
            pos = url.length();
        if (pos < lastPos + 1)
            break;

        std::string value = urlUnescape(url.substr(lastPos, pos - lastPos));
        lastPos = pos + 1;

        dict.emplace(key, value);
    } while (lastPos < url.length());

    return dict;
}

void dictMerge(std::map<std::string, std::string>& result, const std::map<std::string, std::string>& source)
{
    for (auto&& [key, value] : source) {
        result.emplace(key, value);
    }
}

std::string mimeTypesToCsv(const std::vector<std::string>& mimeTypes)
{
    return mimeTypes.empty() ? "" : fmt::format("http-get:*:{}:*", fmt::join(mimeTypes, ":*,http-get:*:"));
}

std::string readTextFile(const fs::path& path)
{
#ifdef __linux__
    auto f = std::fopen(path.c_str(), "rte");
#else
    auto f = std::fopen(path.c_str(), "rt");
#endif
    if (!f) {
        throw_std_runtime_error("Could not open {}: {}", path.c_str(), std::strerror(errno));
    }
    std::ostringstream buf;
    std::array<char, 1024> buffer;
    std::size_t bytesRead;
    while ((bytesRead = std::fread(buffer.data(), 1, buffer.size(), f)) > 0) {
        buf << std::string(buffer.data(), bytesRead);
    }
    std::fclose(f);
    return buf.str();
}

void writeTextFile(const fs::path& path, std::string_view contents)
{
#ifdef __linux__
    auto f = std::fopen(path.c_str(), "wte");
#else
    auto f = std::fopen(path.c_str(), "wt");
#endif
    if (!f) {
        throw_std_runtime_error("Could not open {}: {}", path.c_str(), std::strerror(errno));
    }

    std::size_t bytesWritten = std::fwrite(contents.data(), 1, contents.length(), f);
    if (bytesWritten < contents.length()) {
        std::fclose(f);

        throw_std_runtime_error("Error writing to {}: {}", path.c_str(), std::strerror(errno));
    }
    std::fclose(f);
}

std::optional<std::vector<std::byte>> readBinaryFile(const fs::path& path)
{
    static_assert(sizeof(std::byte) == sizeof(std::ifstream::char_type));

    auto file = std::ifstream(path, std::ios::in | std::ios::binary);
    if (!file)
        return std::nullopt;

    auto& fb = *file.rdbuf();

    // Somewhat portable way to read file.
    // sgetn loops internally, so we need to check only the final result.
    // Also assume file size doesn't change while reading,
    // and no line conversion happens (therefore lseek returns result close to file size).
    auto size = fb.pubseekoff(0, std::ios::end);
    if (size < 0)
        throw_std_runtime_error("Can't determine file size of {}", path.c_str());

    fb.pubseekoff(0, std::ios::beg);

    auto result = std::optional<std::vector<std::byte>>(size);
    size = fb.sgetn(reinterpret_cast<char*>(result->data()), size);
    if (size < 0 || !file)
        throw_std_runtime_error("Failed to read from file {}", path.c_str());

    result->resize(size);

    return result;
}

void writeBinaryFile(const fs::path& path, const std::byte* data, std::size_t size)
{
    static_assert(sizeof(std::byte) == sizeof(std::ifstream::char_type));

    auto file = std::ofstream(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file)
        throw_std_runtime_error("Failed to open {}", path.c_str());

    file.rdbuf()->sputn(reinterpret_cast<const char*>(data), size);

    if (!file)
        throw_std_runtime_error("Failed to write to file {}", path.c_str());
}

std::string renderProtocolInfo(std::string_view mimetype, std::string_view protocol, std::string_view extend)
{
    if (!mimetype.empty() && !protocol.empty()) {
        if (!extend.empty())
            return fmt::format("{}:*:{}:{}", protocol, mimetype, extend);
        return fmt::format("{}:*:{}:*", protocol, mimetype);
    }

    return "http-get:*:*:*";
}

std::string getMTFromProtocolInfo(std::string_view protocol)
{
    std::vector<std::string> parts = splitString(protocol, ':');
    if (parts.size() > 2)
        return parts[2];

    return {};
}

std::string getProtocol(const std::string& protocolInfo)
{
    auto pos = protocolInfo.find(':');
    return (pos == std::string::npos || pos == 0) ? PROTOCOL : protocolInfo.substr(0, pos);
}

std::string millisecondsToHMSF(int milliseconds)
{
    auto ms = milliseconds % 1000;
    milliseconds /= 1000;

    auto s = milliseconds % 60;
    milliseconds /= 60;

    auto m = milliseconds % 60;
    auto h = milliseconds / 60;

    return fmt::format("{:01}:{:02}:{:02}.{:03}", h, m, s, ms);
}

int HMSFToMilliseconds(std::string_view time)
{
    if (time.empty()) {
        log_warning("Could not convert time representation to seconds!");
        return 0;
    }

    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int ms = 0;
    sscanf(time.data(), "%d:%d:%d.%d", &hours, &minutes, &seconds, &ms);

    return ((hours * 3600) + (minutes * 60) + seconds) * 1000 + ms;
}

bool checkResolution(std::string_view resolution, int* x, int* y)
{
    if (x)
        *x = 0;

    if (y)
        *y = 0;

    std::vector<std::string> parts = splitString(resolution, 'x');
    if (parts.size() != 2)
        return false;

    if (!parts[0].empty() && !parts[1].empty()) {
        int _x = std::stoi(parts[0]);
        int _y = std::stoi(parts[1]);

        if ((_x > 0) && (_y > 0)) {
            if (x)
                *x = _x;

            if (y)
                *y = _y;

            return true;
        }
    }

    return false;
}

std::string escape(std::string string, char escapeChar, char toEscape)
{
    std::ostringstream buf;
    auto len = string.length();

    bool possibleMoreEsc = true;
    bool possibleMoreChar = true;

    std::size_t last = 0;
    do {
        auto nextEsc = std::string::npos;
        if (possibleMoreEsc) {
            nextEsc = string.find(escapeChar, last);
            if (nextEsc == std::string::npos)
                possibleMoreEsc = false;
        }

        auto next = std::string::npos;
        if (possibleMoreChar) {
            next = string.find(toEscape, last);
            if (next == std::string::npos)
                possibleMoreChar = false;
        }

        if (next == std::string::npos || (nextEsc != std::string::npos && nextEsc < next)) {
            next = nextEsc;
        }

        if (next == std::string::npos)
            next = len;
        auto cpLen = int(next - last);
        if (cpLen > 0) {
            buf.write(&string[last], cpLen);
        }
        if (next < len) {
            buf << '\\';
            buf << string.at(next);
        }
        last = next;
        last++;
    } while (last < len);

    return buf.str();
}

std::string unescape(std::string string, char escape)
{
    std::ostringstream buf;
    auto len = string.length();

    auto last = std::string::npos;
    do {
        auto next = string.find(escape, last + 1);
        if (next == std::string::npos)
            next = len;
        if (last == std::string::npos)
            last = 0;
        int cpLen = next - last;
        if (cpLen > 0)
            buf.write(&string[last], cpLen);
        last = next;
        last++;
    } while (last < len);

    return buf.str();
}

std::string fallbackString(const std::string& first, const std::string& fallback)
{
    return first.empty() ? fallback : first;
}

unsigned int stringHash(std::string_view str)
{
    return std::accumulate(str.begin(), str.end(), 5381U, [](auto h, auto ch) { return ((h << 5) + h) ^ ch; });
}

std::string getValueOrDefault(const std::vector<std::pair<std::string, std::string>>& m, const std::string& key, const std::string& defval)
{
    return getValueOrDefault<std::string, std::string>(m, key, defval);
}

std::string getValueOrDefault(const std::map<std::string, std::string>& m, const std::string& key, const std::string& defval)
{
    return getValueOrDefault<std::string, std::string>(m, key, defval);
}

std::chrono::seconds currentTime()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
}

std::chrono::milliseconds currentTimeMS()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
}

std::chrono::milliseconds getDeltaMillis(std::chrono::milliseconds ms)
{
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    return now - ms;
}

std::chrono::milliseconds getDeltaMillis(std::chrono::milliseconds first, std::chrono::milliseconds second)
{
    return second - first;
}

std::string ipToInterface(std::string_view ip)
{
    if (ip.empty()) {
        return {};
    }

    log_warning("Please provide an interface name instead! LibUPnP does not support specifying an IP in current versions.");
    log_info("Attempting to map {} to an interface", ip);

    struct ifaddrs *ifaddr, *ifa;
    int family, s, n;
    char host[NI_MAXHOST];

    if (getifaddrs(&ifaddr) == -1) {
        log_error("Could not getifaddrs: {}", std::strerror(errno));
    }

    for (ifa = ifaddr, n = 0; ifa; ifa = ifa->ifa_next, n++) {
        if (!ifa->ifa_addr)
            continue;

        family = ifa->ifa_addr->sa_family;
        std::string name = ifa->ifa_name;

        if (family == AF_INET || family == AF_INET6) {
            s = getnameinfo(ifa->ifa_addr,
                (family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6),
                host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST);
            if (s != 0) {
                log_error("getnameinfo() failed: {}", gai_strerror(s));
                return {};
            }

            std::string ipaddr = host;
            // IPv6 link locals come back as fe80::351d:d7f4:6b17:3396%eth0
            if (startswith(ipaddr, ip)) {
                return name;
            }
        }
    }

    freeifaddrs(ifaddr);
    log_warning("Failed to find interface for IP: {}", ip);
    return {};
}

bool validateYesNo(std::string_view value)
{
    return value == "yes" || value == "no";
}

std::vector<std::string> populateCommandLine(const std::string& line,
    const std::string& in,
    const std::string& out,
    const std::string& range,
    const std::string& title)
{
    log_debug("Template: '{}', in: '{}', out: '{}', range: '{}', title: '{}'", line, in, out, range, title);
    std::vector<std::string> params = splitString(line, ' ');
    if (in.empty() && out.empty())
        return params;

    for (auto&& param : params) {
        auto inPos = param.find("%in");
        if (inPos != std::string::npos) {
            std::string newParam = param.replace(inPos, 3, in);
        }

        auto outPos = param.find("%out");
        if (outPos != std::string::npos) {
            std::string newParam = param.replace(outPos, 4, out);
        }

        auto rangePos = param.find("%range");
        if (rangePos != std::string::npos) {
            std::string newParam = param.replace(rangePos, 6, range);
        }

        auto titlePos = param.find("%title");
        if (titlePos != std::string::npos) {
            std::string newParam = param.replace(titlePos, 6, title);
        }
    }

    return params;
}

// The tempName() function is borrowed from gfileutils.c from the glibc package

/* gfileutils.c - File utility functions
 *
 *  Copyright 2000 Red Hat, Inc.
 *
 * GLib is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * GLib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GLib; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *   Boston, MA 02111-1307, USA.
 */
/*
 * create_temp_file based on the mkstemp implementation from the GNU C library.
 * Copyright (C) 1991,92,93,94,95,96,97,98,99 Free Software Foundation, Inc.
 */
// tempName is based on create_temp_file, see (C) above
fs::path tempName(const fs::path& leadPath, char* tmpl)
{
    char* xxxxxx;
    int count;
    static constexpr char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static constexpr int nletters = sizeof(letters) - 1;
    long value;
    struct timespec ts;
    static int counter = 0;
    struct stat statbuf;
    int ret;

    /* find the last occurrence of "XXXXXX" */
    xxxxxx = strstr(tmpl, "XXXXXX");

    if (!xxxxxx || std::strncmp(xxxxxx, "XXXXXX", 6) != 0) {
        return {};
    }

    /* Get some more or less random data.  */
    clock_gettime(CLOCK_REALTIME, &ts);
    value = (ts.tv_nsec ^ ts.tv_sec) + counter++;

    for (count = 0; count < 100; value += 7777, ++count) {
        long v = value;

        /* Fill in the random bits.  */
        xxxxxx[0] = letters[v % nletters];
        v /= nletters;
        xxxxxx[1] = letters[v % nletters];
        v /= nletters;
        xxxxxx[2] = letters[v % nletters];
        v /= nletters;
        xxxxxx[3] = letters[v % nletters];
        v /= nletters;
        xxxxxx[4] = letters[v % nletters];
        v /= nletters;
        xxxxxx[5] = letters[v % nletters];

        fs::path check = leadPath / tmpl;
        ret = stat(check.c_str(), &statbuf);
        if (ret != 0) {
            if ((errno == ENOENT) || (errno == ENOTDIR))
                return check;

            return {};
        }
    }

    /* We got out of the loop because we ran out of combinations to try.  */
    return {};
}

bool isTheora(const fs::path& oggFilename)
{
    char buffer[7];

#ifdef __linux__
    auto f = std::fopen(oggFilename.c_str(), "rbe");
#else
    auto f = std::fopen(oggFilename.c_str(), "rb");
#endif
    if (!f) {
        throw_std_runtime_error("Error opening {}: {}", oggFilename.c_str(), std::strerror(errno));
    }

    if (std::fread(buffer, 1, 4, f) != 4) {
        std::fclose(f);
        throw_std_runtime_error("Error reading {}", oggFilename.c_str());
    }

    if (std::memcmp(buffer, "OggS", 4) != 0) {
        std::fclose(f);
        return false;
    }

    if (std::fseek(f, 28, SEEK_SET) != 0) {
        std::fclose(f);
        throw_std_runtime_error("Incomplete file {}", oggFilename.c_str());
    }

    if (std::fread(buffer, 1, 7, f) != 7) {
        std::fclose(f);
        throw_std_runtime_error("Error reading {}", oggFilename.c_str());
    }

    if (std::memcmp(buffer, "\x80theora", 7) != 0) {
        std::fclose(f);
        return false;
    }

    std::fclose(f);
    return true;
}

fs::path getLastPath(const fs::path& path)
{
    auto it = std::prev(path.end()); // filename
    if (it != path.end())
        it = std::prev(it); // last path
    if (it != path.end())
        return *it;

    return {};
}

ssize_t getValidUTF8CutPosition(std::string_view str, ssize_t cutpos)
{
    ssize_t pos = -1;
    auto len = str.length();

    if ((len == 0) || (cutpos > ssize_t(len)))
        return pos;

#if 0
    printf("Character at cut position: %0x\n", char(str.at(cutpos)));
    printf("Character at cut-1 position: %0x\n", char(str.at(cutpos - 1)));
    printf("Character at cut-2 position: %0x\n", char(str.at(cutpos - 2)));
    printf("Character at cut-3 position: %0x\n", char(str.at(cutpos - 3)));
#endif

    // > 0x7f, we are dealing with a non-ascii character
    if (str.at(cutpos) & 0x80) {
        // check if we are at byte 2
        if (((cutpos - 1) >= 0) && (((str.at(cutpos - 1) & 0xc2) == 0xc2) || ((str.at(cutpos - 1) & 0xe2) == 0xe2) || ((str.at(cutpos - 1) & 0xf0) == 0xf0)))
            pos = cutpos - 1;
        // check if we are at byte 3
        else if (((cutpos - 2) >= 0) && (((str.at(cutpos - 2) & 0xe2) == 0xe2) || ((str.at(cutpos - 2) & 0xf0) == 0xf0)))
            pos = cutpos - 2;
        // we must be at byte 4 then...
        else if ((cutpos - 3) >= 0)
            pos = cutpos - 3;
    } else
        pos = cutpos;

    return pos;
}

std::string getDLNAprofileString(const std::shared_ptr<Config>& config, const std::string& contentType, const std::string& vCodec, const std::string& aCodec)
{
    // get profiles from <contenttype-dlnaprofile> mappings
    auto mappings = config->getDictionaryOption(CFG_IMPORT_MAPPINGS_CONTENTTYPE_TO_DLNAPROFILE_LIST);
    auto profile = getValueOrDefault(mappings, fmt::format("{}-{}-{}", contentType, vCodec, aCodec), "");
    if (profile.empty())
        profile = getValueOrDefault(mappings, contentType, "");

    return profile.empty() ? "" : fmt::format("{}={};", UPNP_DLNA_PROFILE, profile);
}

std::string getDLNAContentHeader(const std::shared_ptr<Config>& config, const std::string& contentType, const std::string& vCodec, const std::string& aCodec)
{
    std::string contentParameter = getDLNAprofileString(config, contentType, vCodec, aCodec);
    return fmt::format("{}{}={};{}={};{}={}", contentParameter, //
        UPNP_DLNA_OP, UPNP_DLNA_OP_SEEK_RANGE, //
        UPNP_DLNA_CONVERSION_INDICATOR, UPNP_DLNA_NO_CONVERSION, //
        UPNP_DLNA_FLAGS, UPNP_DLNA_ORG_FLAGS_AV);
}

std::string getDLNATransferHeader(const std::shared_ptr<Config>& config, std::string_view mimeType)
{
    if (startswith(mimeType, "image")) {
        return UPNP_DLNA_TRANSFER_MODE_INTERACTIVE;
    }

    if (startswith(mimeType, "audio") || startswith(mimeType, "video")) {
        return UPNP_DLNA_TRANSFER_MODE_STREAMING;
    }

    return {};
}

#ifndef HAVE_FFMPEG
std::string getAVIFourCC(std::string_view aviFilename)
{
#define FCC_OFFSET 0xbc

#ifdef __linux__
    auto f = std::fopen(aviFilename.data(), "rbe");
#else
    auto f = std::fopen(aviFilename.data(), "rb");
#endif
    if (!f)
        throw_std_runtime_error("Could not open file {}: {}", aviFilename, std::strerror(errno));

    auto buffer = new char[FCC_OFFSET + 6];

    std::size_t rb = std::fread(buffer, 1, FCC_OFFSET + 4, f);
    std::fclose(f);
    if (rb != FCC_OFFSET + 4) {
        delete[] buffer;
        throw_std_runtime_error("Could not read header of {}: {}", aviFilename, std::strerror(errno));
    }

    buffer[FCC_OFFSET + 5] = '\0';

    if (std::strncmp(buffer, "RIFF", 4) != 0) {
        delete[] buffer;
        return {};
    }

    if (std::strncmp(buffer + 8, "AVI ", 4) != 0) {
        delete[] buffer;
        return {};
    }

    auto fourcc = std::string(buffer + FCC_OFFSET, 4);
    delete[] buffer;

    return fourcc;
}
#endif

std::string getHostName(const struct sockaddr* addr)
{
    char hoststr[NI_MAXHOST];
    char portstr[NI_MAXSERV];
    int len = addr->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    int ret = getnameinfo(addr, len, hoststr, sizeof(hoststr), portstr, sizeof(portstr), NI_NOFQDN);
    if (ret != 0) {
        log_debug("could not determine getnameinfo: {}", std::strerror(errno));
    }

    return hoststr;
}

int sockAddrCmpAddr(const struct sockaddr* sa, const struct sockaddr* sb)
{
    if (sa->sa_family != sb->sa_family)
        return sa->sa_family - sb->sa_family;

    if (sa->sa_family == AF_INET) {
        return SOCK_ADDR_IN_ADDR(sa).s_addr - SOCK_ADDR_IN_ADDR(sb).s_addr;
    }

    if (sa->sa_family == AF_INET6) {
        return std::memcmp(&(SOCK_ADDR_IN6_ADDR(sa)), &(SOCK_ADDR_IN6_ADDR(sb)), sizeof(SOCK_ADDR_IN6_ADDR(sa)));
    }

    throw_std_runtime_error("Unsupported address family: {}", sa->sa_family);
}

std::string sockAddrGetNameInfo(const struct sockaddr* sa)
{
    char hoststr[NI_MAXHOST];
    char portstr[NI_MAXSERV];
    int len = sa->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    int ret = getnameinfo(sa, len, hoststr, sizeof(hoststr), portstr, sizeof(portstr), NI_NUMERICHOST | NI_NUMERICSERV);
    if (ret != 0) {
        throw_std_runtime_error("could not determine getnameinfo: {}", std::strerror(errno));
    }

    return fmt::format("{}:{}", hoststr, portstr);
}

#ifdef SOPCAST
/// \brief
int find_local_port(in_port_t rangeMin, in_port_t rangeMax)
{
    int fd;
    int retryCount = 0;
    in_port_t port;
    struct sockaddr_in serverAddr;
    struct hostent* server;

    if (rangeMin > rangeMax) {
        log_error("min port range > max port range!");
        return -1;
    }

    std::mt19937 rng;
    std::uniform_int_distribution<in_port_t> gen(rangeMin, rangeMax);

    do {
        port = gen(rng);

        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            log_error("could not determine free port: error creating socket: {}", std::strerror(errno));
            return -1;
        }

        server = gethostbyname("127.0.0.1");
        if (!server) {
            log_error("could not resolve localhost");
            close(fd);
            return -1;
        }

        serverAddr = {};
        serverAddr.sin_family = AF_INET;
        std::memcpy(&serverAddr.sin_addr.s_addr, server->h_addr, server->h_length);
        serverAddr.sin_port = htons(port);

        if (connect(fd, reinterpret_cast<struct sockaddr*>(&serverAddr),
                sizeof(serverAddr))
            == -1) {
            close(fd);
            return port;
        }

        close(fd);

        retryCount++;
    } while (retryCount < std::numeric_limits<in_port_t>::max());

    log_error("Could not find free port on localhost");

    return -1;
}
#endif
