#include <assert.h>
#include <iostream>
#include "led_policy.h"

static_assert(karute_led::asciiIndicatorKey(1, 'c'));
static_assert(!karute_led::asciiIndicatorKey(1, 'v'));
static_assert(karute_led::asciiIndicatorKey(2, 'e'));
static_assert(karute_led::asciiIndicatorKey(2, 'E'));
static_assert(!karute_led::asciiIndicatorKey(2, 'a'));
static_assert(karute_led::altIndicatorKey(4, 0x59));
static_assert(!karute_led::altIndicatorKey(4, 0x5a));
static_assert(karute_led::wifiDisconnectedRed(0));
static_assert(karute_led::wifiDisconnectedRed(399));
static_assert(!karute_led::wifiDisconnectedRed(400));
static_assert(!karute_led::wifiDisconnectedRed(1999));
static_assert(karute_led::wifiDisconnectedRed(2000));

int main() {
  assert(karute_led::asciiIndicatorKey(1, 'c'));
  assert(!karute_led::asciiIndicatorKey(1, 'v'));
  assert(!karute_led::asciiIndicatorKey(1, '1'));

  assert(karute_led::asciiIndicatorKey(2, 'e'));
  assert(karute_led::asciiIndicatorKey(2, 'E'));
  assert(!karute_led::asciiIndicatorKey(2, 'a'));
  assert(!karute_led::asciiIndicatorKey(2, '1'));

  assert(karute_led::altIndicatorKey(4, 0x59));
  assert(!karute_led::altIndicatorKey(4, 0x5a));
  assert(!karute_led::altIndicatorKey(4, 0x62));
  assert(!karute_led::altIndicatorKey(1, 0x59));
  assert(karute_led::wifiDisconnectedRed(0));
  assert(karute_led::wifiDisconnectedRed(399));
  assert(!karute_led::wifiDisconnectedRed(400));
  assert(!karute_led::wifiDisconnectedRed(1999));
  assert(karute_led::wifiDisconnectedRed(2000));
  std::cout << "PASS: VUC c, GBK keypad 1 and ENG E/e are the only sampled keys\n";
}
