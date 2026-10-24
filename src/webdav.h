// WebDAV share of the SD card at http://<watch>/dav so Windows File Explorer
// can map it as a network drive ("Map network drive" -> http://<ip>/dav).
#pragma once
#include <WebServer.h>

// Headers the DAV handler needs WebServer to keep
extern const char* const kDavHeaders[];
extern const size_t kDavHeaderCount;

RequestHandler* webdavHandler(bool (*authorize)());
