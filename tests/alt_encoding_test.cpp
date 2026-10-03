#include <assert.h>
#include <string>
#include <utility>
#include <vector>
#include <iostream>
#include "alt_encoding.h"

using namespace karute_alt;

uint32_t decoded(const std::string &text) {
  size_t index = 0;
  uint32_t cp = 0;
  assert(decode(text.data(), text.size(), index, cp));
  assert(index == text.size());
  return cp;
}
void invalid(const std::string &text) {
  size_t index = 0;
  uint32_t cp = 0;
  assert(!decode(text.data(), text.size(), index, cp));
}
int main() {
  assert(decoded("中") == 0x4e2d);
  assert(decoded("℃") == 0x2103);
  assert(decoded("𠮷") == 0x20bb7);
  assert(decoded("😀") == 0x1f600);
  assert(decoded("A") == 65);
  invalid(std::string("\xc0\xaf", 2));  // Overlong.
  invalid(std::string("\xe0\x80\xaf", 3));
  invalid(std::string("\xed\xa0\x80", 3));  // Surrogate.
  invalid(std::string("\xf4\x90\x80\x80", 4));  // > U+10FFFF.
  invalid(std::string("\xe4\xb8", 2));  // Truncated.
  invalid(std::string("\x80", 1));
  invalid(std::string("\xe4\x41\xad", 3));
  invalid("");

  const std::pair<uint32_t, uint16_t> expected[] = {
    {0x4e2d, 0xd6d0}, {0x6587, 0xcec4}, {0x4f60, 0xc4e3},
    {0x597d, 0xbac3}, {0x2103, 0xa1e6}, {0x00b1, 0xa1c0},
    {0x03bc, 0xa6cc}, {0x03b1, 0xa6c1}, {0x03b2, 0xa6c2},
    {0x00d7, 0xa1c1}, {0xff0c, 0xa3ac}, {0x3002, 0xa1a3},
  };
  uint16_t value;
  for (auto pair : expected) {
    assert(outputCode(Mode::GBK, pair.first, value));
    assert(value == pair.second);
    assert(outputCode(Mode::UNICODE_DECIMAL, pair.first, value));
    assert(value == pair.first);
  }
  // Binary search must find every generated mapping, without truncation.
  uint32_t previous = 0;
  for (auto packed : kGbkPairs) {
    assert((packed >> 16) > previous);
    previous = packed >> 16;
    assert(gbkCode(previous, value));
    assert(value == (packed & 0xffff));
  }
  assert(sizeof(kGbkPairs) / sizeof(kGbkPairs[0]) == 21791);
  for (uint32_t cp = 32; cp <= 126; ++cp) {
    assert(outputCode(Mode::GBK, cp, value) && value == cp);
    assert(outputCode(Mode::UNICODE_DECIMAL, cp, value) && value == cp);
  }
  assert(!outputCode(Mode::GBK, 0x1f600, value));
  assert(!outputCode(Mode::UNICODE_DECIMAL, 0x20bb7, value));
  assert(!outputCode(Mode::GBK, 0x00a5, value));  // Yen not in strict GBK.
  assert(!outputCode(Mode::UNICODE_DECIMAL, 0xd800, value));
  assert(!outputCode(Mode::GBK, 0, value));
  assert(!outputCode(Mode::UNICODE_DECIMAL, 0x1b, value));
  assert(!outputCode(Mode::UNICODE_DECIMAL, 0x7f, value));
  assert(!outputCode(Mode::UNICODE_DECIMAL, 0x80, value));
  for (auto cp : {'\n', '\r', '\t', '\b'})
    assert(outputCode(Mode::GBK, cp, value) && value == cp);

  assert(keypadUsage('0') == 0x62);
  for (char digit = '1'; digit <= '9'; ++digit)
    assert(keypadUsage(digit) == 0x59 + (digit - '1'));
  assert(keypadUsage('a') == 0);
  assert(decimalDigits(54992) == 5);
  assert(decimalDigits(65) == 2);

  std::vector<std::pair<uint8_t, uint8_t>> reports;
  uint32_t time = 0;
  bool stop = false;
  auto send = [&](uint8_t modifier, uint8_t usage) {
    reports.emplace_back(modifier, usage);
    return true;
  };
  auto wait = [&](uint32_t ms) { time += ms; stop = true; };
  assert(emitCode(54992, 5, 5, 10, send, wait));
  assert(stop);  // Even a stop during the FIRST key finishes this one code.
  const std::vector<std::pair<uint8_t, uint8_t>> required = {
    {4, 0}, {4, 0x5d}, {4, 0}, {4, 0x5c}, {4, 0},
    {4, 0x61}, {4, 0}, {4, 0x61}, {4, 0}, {4, 0x5a}, {4, 0}, {0, 0}
  };
  assert(reports == required);  // Includes release between the two 9 digits.
  assert(time == characterEstimateMs(54992, 5, 5, 10));
  assert(time == 65);
  reports.clear(); time = 0;
  std::vector<uint32_t> waits;
  auto timedWait = [&](uint32_t ms) { waits.push_back(ms); time += ms; };
  assert(emitTimedCode(54992, 2, 3, 7, 11, 13, send, timedWait));
  assert((waits == std::vector<uint32_t>{7, 2, 3, 2, 3, 2, 3, 2, 3, 2, 11, 13}));
  assert(time == timedEstimateMs(54992, 2, 3, 7, 11, 13));
  assert(time == 53);
  reports.clear(); time = 0;
  assert(emitCode('\n', 5, 5, 10, send, wait));
  assert(reports.size() == 2 && reports[0] == std::make_pair(uint8_t(0), uint8_t(0x28)));
  assert(reports[1] == std::make_pair(uint8_t(0), uint8_t(0)));
  assert(time == 15);
  // Each possible failed transfer must be returned to the caller promptly.
  for (unsigned fail = 0; fail < required.size(); ++fail) {
    unsigned calls = 0;
    assert(!emitCode(54992, 1, 1, 1,
        [&](uint8_t, uint8_t) { return calls++ != fail; }, [](uint32_t) {}));
    assert(calls == fail + 1);
  }
  std::cout << "PASS: strict UTF-8, 21791 GBK mappings, mixed symbols, keypad reports, repeated digits, boundary stop and transport failures\n";
}
