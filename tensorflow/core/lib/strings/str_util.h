/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_CORE_LIB_STRINGS_STR_UTIL_H_
#define TENSORFLOW_CORE_LIB_STRINGS_STR_UTIL_H_

#include <functional>
#include <string>
#include <vector>
#include "absl/strings/string_view.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/types.h"

// Basic string utility routines
namespace tensorflow {
namespace str_util {

// Returns a version of 'src' where unprintable characters have been
// escaped using C-style escape sequences.
string CEscape(absl::string_view src);

// Copies "source" to "dest", rewriting C-style escape sequences --
// '\n', '\r', '\\', '\ooo', etc -- to their ASCII equivalents.
//
// Errors: Sets the description of the first encountered error in
// 'error'. To disable error reporting, set 'error' to NULL.
//
// NOTE: Does not support \u or \U!
bool CUnescape(absl::string_view source, string* dest, string* error);

// Removes any trailing whitespace from "*s".
void StripTrailingWhitespace(string* s);

// Removes leading ascii_isspace() characters.
// Returns number of characters removed.
size_t RemoveLeadingWhitespace(absl::string_view* text);

// Removes trailing ascii_isspace() characters.
// Returns number of characters removed.
size_t RemoveTrailingWhitespace(absl::string_view* text);

// Removes leading and trailing ascii_isspace() chars.
// Returns number of chars removed.
size_t RemoveWhitespaceContext(absl::string_view* text);

// Consume a leading positive integer value.  If any digits were
// found, store the value of the leading unsigned number in "*val",
// advance "*s" past the consumed number, and return true.  If
// overflow occurred, returns false.  Otherwise, returns false.
bool ConsumeLeadingDigits(absl::string_view* s, uint64* val);

// Consume a leading token composed of non-whitespace characters only.
// If *s starts with a non-zero number of non-whitespace characters, store
// them in *val, advance *s past them, and return true.  Else return false.
bool ConsumeNonWhitespace(absl::string_view* s, absl::string_view* val);

// If "*s" starts with "expected", consume it and return true.
// Otherwise, return false.
bool ConsumePrefix(absl::string_view* s, absl::string_view expected);

// If "*s" ends with "expected", remove it and return true.
// Otherwise, return false.
bool ConsumeSuffix(absl::string_view* s, absl::string_view expected);

// Return lower-cased version of s.
string Lowercase(absl::string_view s);

// Return upper-cased version of s.
string Uppercase(absl::string_view s);

// Converts "^2ILoveYou!" to "i_love_you_". More specifically:
// - converts all non-alphanumeric characters to underscores
// - replaces each occurrence of a capital letter (except the very
//   first character and if there is already an '_' before it) with '_'
//   followed by this letter in lower case
// - Skips leading non-alpha characters
// This method is useful for producing strings matching "[a-z][a-z0-9_]*"
// as required by OpDef.ArgDef.name. The resulting string is either empty or
// matches this regex.
string ArgDefCase(absl::string_view s);

// Capitalize first character of each word in "*s".  "delimiters" is a
// set of characters that can be used as word boundaries.
void TitlecaseString(string* s, absl::string_view delimiters);

// Replaces the first occurrence (if replace_all is false) or all occurrences
// (if replace_all is true) of oldsub in s with newsub.
string StringReplace(absl::string_view s, absl::string_view oldsub,
                     absl::string_view newsub, bool replace_all);

// Join functionality
template <typename T>
string Join(const T& s, const char* sep);

// A variant of Join where for each element of "s", f(&dest_string, elem)
// is invoked (f is often constructed with a lambda of the form:
//   [](string* result, ElemType elem)
template <typename T, typename Formatter>
string Join(const T& s, const char* sep, Formatter f);

struct AllowEmpty {
  bool operator()(absl::string_view sp) const { return true; }
};
struct SkipEmpty {
  bool operator()(absl::string_view sp) const { return !sp.empty(); }
};
struct SkipWhitespace {
  bool operator()(absl::string_view sp) const {
    RemoveTrailingWhitespace(&sp);
    return !sp.empty();
  }
};

// Split strings using any of the supplied delimiters. For example:
// Split("a,b.c,d", ".,") would return {"a", "b", "c", "d"}.
std::vector<string> Split(absl::string_view text, absl::string_view delims);

template <typename Predicate>
std::vector<string> Split(absl::string_view text, absl::string_view delims,
                          Predicate p);

// Split "text" at "delim" characters, and parse each component as
// an integer.  If successful, adds the individual numbers in order
// to "*result" and returns true.  Otherwise returns false.
bool SplitAndParseAsInts(absl::string_view text, char delim,
                         std::vector<int32>* result);
bool SplitAndParseAsInts(absl::string_view text, char delim,
                         std::vector<int64>* result);
bool SplitAndParseAsFloats(absl::string_view text, char delim,
                           std::vector<float>* result);

// StartsWith()
//
// Returns whether a given string `text` begins with `prefix`.
bool StartsWith(absl::string_view text, absl::string_view prefix);

// EndsWith()
//
// Returns whether a given string `text` ends with `suffix`.
bool EndsWith(absl::string_view text, absl::string_view suffix);

// StrContains()
//
// Returns whether a given string `haystack` contains the substring `needle`.
bool StrContains(absl::string_view haystack, absl::string_view needle);

// ------------------------------------------------------------------
// Implementation details below
template <typename T>
string Join(const T& s, const char* sep) {
  string result;
  bool first = true;
  for (const auto& x : s) {
    tensorflow::strings::StrAppend(&result, (first ? "" : sep), x);
    first = false;
  }
  return result;
}

template <typename T>
class Formatter {
 public:
  Formatter(std::function<void(string*, T)> f) : f_(f) {}
  void operator()(string* out, const T& t) { f_(out, t); }

 private:
  std::function<void(string*, T)> f_;
};

template <typename T, typename Formatter>
string Join(const T& s, const char* sep, Formatter f) {
  string result;
  bool first = true;
  for (const auto& x : s) {
    if (!first) {
      result.append(sep);
    }
    f(&result, x);
    first = false;
  }
  return result;
}

inline std::vector<string> Split(absl::string_view text,
                                 absl::string_view delims) {
  return Split(text, delims, AllowEmpty());
}

template <typename Predicate>
std::vector<string> Split(absl::string_view text, absl::string_view delims,
                          Predicate p) {
  std::vector<string> result;
  size_t token_start = 0;
  if (!text.empty()) {
    for (size_t i = 0; i < text.size() + 1; i++) {
      if ((i == text.size()) ||
          (delims.find(text[i]) != absl::string_view::npos)) {
        absl::string_view token(text.data() + token_start, i - token_start);
        if (p(token)) {
          result.emplace_back(token);
        }
        token_start = i + 1;
      }
    }
  }
  return result;
}

inline std::vector<string> Split(absl::string_view text, char delim) {
  return Split(text, absl::string_view(&delim, 1));
}

template <typename Predicate>
std::vector<string> Split(absl::string_view text, char delims, Predicate p) {
  return Split(text, absl::string_view(&delims, 1), p);
}

// Returns the length of the given null-terminated byte string 'str'.
// Returns 'string_max_len' if the null character was not found in the first
// 'string_max_len' bytes of 'str'.
size_t Strnlen(const char* str, const size_t string_max_len);

}  // namespace str_util
}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_LIB_STRINGS_STR_UTIL_H_
