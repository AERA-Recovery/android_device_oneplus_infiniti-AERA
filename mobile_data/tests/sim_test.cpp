/* SPDX-License-Identifier: Apache-2.0 */
#include "../sim.hpp"
#include <cassert>
#include <cstdio>
using namespace aera::mobile;
static void Card(Bytes &out,unsigned state,unsigned pin,unsigned retries,bool universal=false) {
  const Bytes card={1,1,3,10,0,1,2,static_cast<uint8_t>(state),0,0,0,0,0,
      static_cast<uint8_t>(universal),static_cast<uint8_t>(pin),static_cast<uint8_t>(retries),10,3,3,10};
  out.insert(out.end(),card.begin(),card.end());
}
int main() {
  Bytes fixture;Put16(fixture,0);Put16(fixture,0xffff);Put16(fixture,0x100);Put16(fixture,0xffff);
  fixture.push_back(2);Card(fixture,2,1,3);Card(fixture,7,3,3);
  const auto parsed=ParseSimCards(fixture);
  assert(parsed.primary==0&&parsed.secondary==0x100&&parsed.cards.size()==2);
  assert(parsed.cards[0].slot==1&&parsed.cards[1].slot==2);
  assert(SimState(parsed.cards[0])=="pin_required"&&parsed.cards[0].retries==3);
  assert(SimState(parsed.cards[1])=="ready");
  auto transient=parsed.cards[0];transient.pin_state=2;transient.state=4;
  assert(SimState(transient)=="initializing");transient.state=7;assert(SimState(transient)=="ready");
  transient.pin_state=4;assert(SimState(transient)=="puk_required");
  transient.pin_state=5;assert(SimState(transient)=="blocked");
  transient.present=false;assert(SimState(transient)=="absent");
  for(size_t i=0;i<fixture.size();++i) {
    bool rejected=false;try{ParseSimCards(Bytes(fixture.begin(),fixture.begin()+i));}
    catch(const std::exception &){rejected=true;}assert(rejected);
  }
  Bytes slots{2};
  for(unsigned logical:{2u,1u}) {
    Put32(slots,2);Put32(slots,1);slots.push_back(logical);slots.push_back(10);
    for(unsigned i=0;i<10;++i)slots.push_back(logical);
  }
  const auto ids=ParseSimIdentifiers(slots);
  assert(ids.at(1)==Bytes(10,1)&&ids.at(2)==Bytes(10,2));
  for(size_t i=0;i<slots.size();++i) {
    bool rejected=false;try{ParseSimIdentifiers(Bytes(slots.begin(),slots.begin()+i));}
    catch(const std::exception &){rejected=true;}assert(rejected);
  }
  assert(ValidPin("1234")&&ValidPin("12345678")&&!ValidPin("123")&&!ValidPin("123456789")&&!ValidPin("12a4"));
  puts("SIM parsing, slot mapping, readiness, retry states and PIN validation passed");
}
