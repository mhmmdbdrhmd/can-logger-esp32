#pragma once
#include "Arduino.h"
#include "WiFi.h"      /* for WiFiClient, which client() hands back */
#define HTTP_GET 1
#define HTTP_POST 2
#define CONTENT_LENGTH_UNKNOWN ((size_t)-1)
/* The upload path, reproduced closely enough that the real signatures are what
   gets compiled. A frame map arrives as multipart/form-data and is streamed to
   the card a chunk at a time, so the handler is called repeatedly with a status
   rather than once with a body - which is the part worth type-checking here. */
enum HTTPUploadStatus {
  UPLOAD_FILE_START = 0,
  UPLOAD_FILE_WRITE,
  UPLOAD_FILE_END,
  UPLOAD_FILE_ABORTED
};
struct HTTPUpload {
  HTTPUploadStatus status = UPLOAD_FILE_START;
  String   filename;
  size_t   currentSize = 0;
  size_t   totalSize   = 0;
  uint8_t  buf[1];
};

class WebServer {
public:
  WebServer(uint16_t){}
  void on(const char*, int, void(*)()) {}
  void on(const char*, int, void(*)(), void(*)()) {}
  HTTPUpload &upload() { static HTTPUpload u; return u; }
  void onNotFound(void(*)()) {}
  void begin() {}
  void handleClient() {}
  void send(int, const char*, const String&) {}
  void send(int, const char*, const char*) {}
  void send_P(int, const char*, const char*) {}
  void setContentLength(size_t) {}
  void sendContent(const String&) {}
  void sendContent(const char*) {}
  void sendContent(const char*, size_t) {}
  /* Both exist on the real WebServer. send(code) with no body is how a
   * handler answers 204; uri() is how onNotFound tells an API path from
   * a navigation. */
  void   send(int) {}
  String uri() { return String("/"); }
  void sendContent_P(const char*) {}
  void sendContent_P(const char*, size_t) {}
  void sendHeader(const char*, const char*) {}
  bool hasArg(const char*) { return false; }
  String arg(const char*) { return String("0"); }
  int args() { return 0; }

  /* The real WebServer hands out the current connection so a handler can ask
   * whether the client is still there. Streaming handlers check it between
   * slices: a write to a client that has gone blocks for up to ten seconds
   * per slice inside WiFiClient::write(), on the loop task.
   *
   * Returned BY VALUE, as the real one is - the copy shares the socket. The
   * shim reports "still connected" so the host tests walk the normal path
   * rather than the give-up path. */
  WiFiClient client() { return WiFiClient(); }
};
