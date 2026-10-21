/* SPDX-License-Identifier: Apache-2.0 */
#include "../modem_features.hpp"
#include <cassert>
#include <cstdio>
using namespace aera::mobile;
static Bytes Record(const std::string &xml) {Bytes data(12288,0);std::copy(xml.begin(),xml.end(),data.begin());return data;}
static bool Rejects(const Bytes &data) {
  try{ParseModemFeatures(data);return false;}catch(const std::exception &){return true;}
}
int main() {
  const std::string valid="<Feature><mdmfeature>true</mdmfeature><emmcid>123456789</emmcid><imeiSv>42</imeiSv></Feature>";
  assert(ParseModemFeatures(Record(valid))==valid);
  assert(Rejects(Bytes(12288,'x')));
  assert(Rejects(Bytes(12288,0)));
  assert(Rejects(Record("<Feature>")));
  assert(Rejects(Record("<Other><mdmfeature>true</mdmfeature></Other>")));
  assert(Rejects(Record("<Feature><mdmfeature>true</mdmfeature><emmcid>123</emmcid></Feature>")));
  assert(Rejects(Record("<Feature><mdmfeature>true</mdmfeature><emmcid>123</emmcid><emmcid>456</emmcid><imeiSv>42</imeiSv></Feature>")));
  assert(Rejects(Record("<Feature><mdmfeature>true</mdmfeature><emmcid>invalid</emmcid><imeiSv>42</imeiSv></Feature>")));
  puts("Modem feature record validation passed");
}
