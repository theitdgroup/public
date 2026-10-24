// Text helpers for feed and metadata strings.
#pragma once
#include <Arduino.h>
#include <time.h>

// Decode XML/HTML entities (&amp; &#8217; &#x2019; ...) to UTF-8.
String decodeEntities(const String& in);
// Remove <tags>, collapse whitespace.
String stripTags(const String& in);
// The built-in LVGL fonts only cover ASCII, so fold typographic punctuation and
// accented Latin letters to their ASCII equivalents and drop anything else.
String asciiFold(const String& in);
// Full clean-up for display: CDATA, entities, tags, folding, trimming, length cap.
String cleanText(const String& in, size_t maxLen = 0);
// Parse RFC 822 ("Tue, 01 Oct 2026 14:05:00 GMT") or ISO 8601 dates to UTC epoch; 0 if unknown.
time_t parseFeedDate(const String& s);
// Percent-encode for URL query strings.
String urlEncode(const String& s);
