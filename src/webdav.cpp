#include "webdav.h"
#include <SD_MMC.h>
#include <time.h>
#include "player.h"
#include "storage.h"

const char* const kDavHeaders[] = {"Depth", "Destination", "Overwrite", "Content-Length", "Lock-Token", "If", "Timeout"};
const size_t kDavHeaderCount = sizeof(kDavHeaders) / sizeof(kDavHeaders[0]);

static const char* kPrefix = "/dav";

static String encodePath(const String& p) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < p.length(); i++) {
    uint8_t c = p[i];
    if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

static String xmlEscape(const String& s) {
  String o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

static String httpDate(time_t t) {
  struct tm tm;
  gmtime_r(&t, &tm);
  char buf[40];
  strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
  return buf;
}

static String isoDate(time_t t) {
  struct tm tm;
  gmtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

// "/dav/Music/a%20b.mp3" or "http://host/dav/..." -> "/Music/a b.mp3"; "" if unsafe
static String sdPath(String uri) {
  int scheme = uri.indexOf("://");
  if (scheme >= 0) {
    int slash = uri.indexOf('/', scheme + 3);
    uri = slash >= 0 ? uri.substring(slash) : "/";
  }
  if (!uri.startsWith(kPrefix)) return "";
  String p = WebServer::urlDecode(uri.substring(strlen(kPrefix)));
  if (!p.startsWith("/")) p = "/" + p;
  while (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
  if (p.indexOf("/../") >= 0 || p.endsWith("/..")) return "";
  return p;
}

static void drainBody(WebServer& s) {
  int len = s.header("Content-Length").toInt();
  NetworkClient c = s.client();
  uint32_t deadline = millis() + 2000;
  uint8_t buf[256];
  while (len > 0 && millis() < deadline) {
    int n = c.read(buf, min(len, (int)sizeof(buf)));
    if (n > 0) len -= n; else delay(1);
  }
}

static bool removeRecursive(const String& path) {
  File f = SD_MMC.open(path);
  if (!f) return false;
  if (!f.isDirectory()) { f.close(); return SD_MMC.remove(path); }
  while (File c = f.openNextFile()) {
    String cp = c.path();
    c.close();
    removeRecursive(cp);
  }
  f.close();
  return SD_MMC.rmdir(path);
}

static bool copyFile(const String& from, const String& to) {
  File in = SD_MMC.open(from, FILE_READ);
  File out = SD_MMC.open(to, FILE_WRITE);
  if (!in || !out || in.isDirectory()) return false;
  uint8_t buf[2048];
  while (int n = in.read(buf, sizeof(buf))) if (out.write(buf, n) != (size_t)n) return false;
  return true;
}

class DavHandler : public RequestHandler {
 public:
  explicit DavHandler(bool (*auth)()) : _auth(auth) {}

  bool canHandle(WebServer&, HTTPMethod m, const String& uri) override {
    if (uri == kPrefix || uri.startsWith(String(kPrefix) + "/")) return true;
    // Windows probes the server root before mapping a folder
    return (m == HTTP_OPTIONS || m == HTTP_PROPFIND) && uri == "/";
  }

  bool canRaw(WebServer&, const String& uri) override { return uri.startsWith(String(kPrefix) + "/"); }

  // Streams PUT bodies straight to the card
  void raw(WebServer& s, const String& uri, HTTPRaw& r) override {
    if (s.method() != HTTP_PUT) return;
    if (r.status == RAW_START) {
      _putError = 0;
      _putPath = sdPath(uri);
      if (!_auth()) { _putError = 401; return; }
      if (!storageAvailable()) { _putError = 503; return; }
      if (_putPath.isEmpty() || _putPath == "/") { _putError = 403; return; }
      _putExisted = SD_MMC.exists(_putPath);
      _putFile = SD_MMC.open(_putPath, FILE_WRITE);
      if (!_putFile) _putError = 409;  // parent folder missing
    } else if (r.status == RAW_WRITE) {
      if (_putFile && _putFile.write(r.buf, r.currentSize) != r.currentSize) _putError = 507;
    } else if (r.status == RAW_END || r.status == RAW_ABORTED) {
      if (_putFile) _putFile.close();
      if (r.status == RAW_ABORTED && !_putError) _putError = 400;
      if (_putError && _putError != 401 && _putError != 503) SD_MMC.remove(_putPath);
    }
  }

  bool handle(WebServer& s, HTTPMethod m, const String& uri) override {
    if (m == HTTP_OPTIONS) {
      s.sendHeader("DAV", "1, 2");
      s.sendHeader("MS-Author-Via", "DAV");
      s.sendHeader("Allow", "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, MOVE, COPY, LOCK, UNLOCK");
      s.send(200, "text/plain", "");
      return true;
    }
    if (!_auth()) {
      drainBody(s);
      s.requestAuthentication(BASIC_AUTH, "ClockHub");
      return true;
    }
    if (m == HTTP_PROPFIND && uri == "/") { drainBody(s); sendRootStub(s); return true; }
    if (!storageAvailable()) {
      drainBody(s);
      s.send(503, "text/plain", storageUsbActive() ? "SD card is in USB drive mode" : "No SD card in the watch");
      return true;
    }
    String path = sdPath(uri);
    if (path.isEmpty()) { s.send(403, "text/plain", "Forbidden"); return true; }

    switch (m) {
      case HTTP_PROPFIND:  drainBody(s); propfind(s, path); break;
      case HTTP_PROPPATCH: drainBody(s); proppatch(s, uri); break;
      case HTTP_GET:
      case HTTP_HEAD:      get(s, path, m == HTTP_HEAD); break;
      case HTTP_PUT:
        s.send(_putError ? _putError : (_putExisted ? 204 : 201), "text/plain", "");
        if (!_putError && path.startsWith("/music")) playerSend(Cmd::Rescan);
        break;
      case HTTP_DELETE:
        if (path == "/") { s.send(403, "text/plain", ""); break; }
        s.send(removeRecursive(path) ? 204 : 404, "text/plain", "");
        break;
      case HTTP_MKCOL:
        drainBody(s);
        if (SD_MMC.exists(path)) s.send(405, "text/plain", "");
        else s.send(SD_MMC.mkdir(path) ? 201 : 409, "text/plain", "");
        break;
      case HTTP_MOVE:
      case HTTP_COPY:      moveCopy(s, path, m == HTTP_MOVE); break;
      case HTTP_LOCK:      drainBody(s); lock(s, uri); break;
      case HTTP_UNLOCK:    s.send(204, "text/plain", ""); break;
      default:             s.send(405, "text/plain", "");
    }
    return true;
  }

 private:
  bool (*_auth)();
  File _putFile;
  String _putPath;
  int _putError = 0;
  bool _putExisted = false;

  static String entry(const String& path, bool dir, size_t size, time_t mtime, bool root) {
    String href = kPrefix + encodePath(path == "/" ? "/" : (dir ? path + "/" : path));
    String name = path == "/" ? "SD card" : path.substring(path.lastIndexOf('/') + 1);
    if (mtime < 946684800) mtime = 946684800;  // FAT dates before 2000 confuse Explorer
    String x = "<D:response><D:href>" + xmlEscape(href) + "</D:href><D:propstat><D:prop>";
    x += "<D:displayname>" + xmlEscape(name) + "</D:displayname>";
    x += "<D:getlastmodified>" + httpDate(mtime) + "</D:getlastmodified>";
    x += "<D:creationdate>" + isoDate(mtime) + "</D:creationdate>";
    if (dir) {
      x += "<D:resourcetype><D:collection/></D:resourcetype>";
      if (root) {
        uint64_t total = SD_MMC.totalBytes(), used = SD_MMC.usedBytes();
        x += "<D:quota-available-bytes>" + String((unsigned long long)(total - used)) + "</D:quota-available-bytes>";
        x += "<D:quota-used-bytes>" + String((unsigned long long)used) + "</D:quota-used-bytes>";
      }
    } else {
      x += "<D:resourcetype/><D:getcontentlength>" + String((unsigned long)size) + "</D:getcontentlength>";
      x += "<D:getcontenttype>application/octet-stream</D:getcontenttype>";
    }
    x += "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>";
    return x;
  }

  static void beginMultistatus(WebServer& s) {
    s.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s.send(207, "application/xml; charset=utf-8", "");
    s.sendContent("<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">");
  }

  static void endMultistatus(WebServer& s) {
    s.sendContent("</D:multistatus>");
    s.sendContent("");
  }

  static void sendRootStub(WebServer& s) {
    beginMultistatus(s);
    s.sendContent("<D:response><D:href>/</D:href><D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype>"
                  "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>");
    endMultistatus(s);
  }

  static void propfind(WebServer& s, const String& path) {
    File f = SD_MMC.open(path);
    if (!f) { s.send(404, "text/plain", "Not found"); return; }
    bool dir = f.isDirectory();
    String depth = s.header("Depth");
    beginMultistatus(s);
    s.sendContent(entry(path, dir, dir ? 0 : f.size(), f.getLastWrite(), path == "/"));
    if (dir && depth != "0") {
      while (File c = f.openNextFile()) {
        String cp = c.path();
        String name = cp.substring(cp.lastIndexOf('/') + 1);
        if (name != "System Volume Information")
          s.sendContent(entry(cp, c.isDirectory(), c.isDirectory() ? 0 : c.size(), c.getLastWrite(), false));
        c.close();
      }
    }
    f.close();
    endMultistatus(s);
  }

  // Windows sets file times/attributes after copying; accept and ignore them
  static void proppatch(WebServer& s, const String& uri) {
    beginMultistatus(s);
    s.sendContent("<D:response><D:href>" + xmlEscape(uri) + "</D:href><D:propstat><D:prop/>"
                  "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>");
    endMultistatus(s);
  }

  static void get(WebServer& s, const String& path, bool headOnly) {
    File f = SD_MMC.open(path);
    if (!f) { s.send(404, "text/plain", "Not found"); return; }
    if (f.isDirectory()) {
      String html = "<html><body><h3>ClockHub SD card" + xmlEscape(path) + "</h3><ul>";
      while (File c = f.openNextFile()) {
        String cp = c.path();
        html += "<li><a href=\"" + xmlEscape(kPrefix + encodePath(cp)) + (c.isDirectory() ? "/" : "") + "\">" +
                xmlEscape(cp.substring(cp.lastIndexOf('/') + 1)) + (c.isDirectory() ? "/" : "") + "</a></li>";
        c.close();
      }
      s.send(200, "text/html", html + "</ul></body></html>");
    } else if (headOnly) {
      s.setContentLength(f.size());
      s.sendHeader("Last-Modified", httpDate(f.getLastWrite()));
      s.send(200, "application/octet-stream", "");
    } else {
      s.sendHeader("Last-Modified", httpDate(f.getLastWrite()));
      s.streamFile(f, "application/octet-stream");
    }
    f.close();
  }

  static void moveCopy(WebServer& s, const String& from, bool move) {
    String to = sdPath(s.header("Destination"));
    if (to.isEmpty() || to == "/" || from == "/") { s.send(403, "text/plain", ""); return; }
    if (!SD_MMC.exists(from)) { s.send(404, "text/plain", ""); return; }
    bool existed = SD_MMC.exists(to);
    if (existed) {
      if (s.header("Overwrite") == "F") { s.send(412, "text/plain", ""); return; }
      removeRecursive(to);
    }
    bool ok = move ? SD_MMC.rename(from, to) : copyFile(from, to);
    s.send(ok ? (existed ? 204 : 201) : 409, "text/plain", "");
  }

  // Explorer insists on locking before writing; hand out a token we don't enforce
  static void lock(WebServer& s, const String& uri) {
    String token = "opaquelocktoken:" + String((uint32_t)esp_random(), HEX) + "-" + String(millis(), HEX);
    s.sendHeader("Lock-Token", "<" + token + ">");
    String x = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>"
               "<D:locktype><D:write/></D:locktype><D:lockscope><D:exclusive/></D:lockscope><D:depth>0</D:depth>"
               "<D:timeout>Second-3600</D:timeout><D:locktoken><D:href>" + token + "</D:href></D:locktoken>"
               "<D:lockroot><D:href>" + xmlEscape(uri) + "</D:href></D:lockroot></D:activelock></D:lockdiscovery></D:prop>";
    s.send(200, "application/xml; charset=utf-8", x);
  }
};

RequestHandler* webdavHandler(bool (*authorize)()) { return new DavHandler(authorize); }
