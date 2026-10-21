/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "qmi.hpp"

namespace aera::mobile {
struct SimCard {
  unsigned slot=0,application=0,type=0,state=0,pin_state=0,retries=0;
  bool present=false,universal=false;
};
struct SimCards {
  uint16_t primary=0xffff,secondary=0xffff;
  std::vector<SimCard> cards;
};
inline SimCards ParseSimCards(const Bytes &bytes) {
  if(bytes.size()<9)throw std::runtime_error("Truncated SIM card status");
  SimCards out;out.primary=U16(bytes.data());out.secondary=U16(bytes.data()+4);
  size_t offset=9;
  auto byte=[&]() -> unsigned {
    if(offset>=bytes.size())throw std::runtime_error("Truncated SIM application status");
    return bytes[offset++];
  };
  for(unsigned slot=0;slot<bytes[8];++slot) {
    const unsigned present=byte(),upin=byte(),retries=byte();byte();byte();
    const unsigned applications=byte();bool added=false;
    for(unsigned app=0;app<applications;++app) {
      SimCard card;card.slot=slot+1;card.application=app;card.present=present==1;
      card.type=byte();card.state=byte();
      for(unsigned i=0;i<4;++i)byte();
      const unsigned aid=byte();for(unsigned i=0;i<aid;++i)byte();
      card.universal=byte()!=0;card.pin_state=byte();card.retries=byte();
      for(unsigned i=0;i<4;++i)byte();
      if(card.universal){card.pin_state=upin;card.retries=retries;}
      if(card.type==1||card.type==2){out.cards.push_back(card);added=true;}
    }
    if(!added){SimCard card;card.slot=slot+1;card.present=present==1;out.cards.push_back(card);}
  }
  return out;
}
inline std::string SimState(const SimCard &card) {
  if(!card.present)return "absent";
  if(card.pin_state==5)return "blocked";
  if(card.pin_state==4||card.state==3||card.state==5)return "puk_required";
  if(card.state==2&&card.pin_state==1)return "pin_required";
  if(card.state==7&&(card.pin_state==2||card.pin_state==3))return "ready";
  if(card.state==6)return "blocked";
  return "initializing";
}
inline bool ValidPin(const std::string &pin) {
  return pin.size()>=4&&pin.size()<=8&&pin.find_first_not_of("0123456789")==std::string::npos;
}
inline std::map<unsigned,Bytes> ParseSimIdentifiers(const Bytes &bytes) {
  if(bytes.empty())throw std::runtime_error("Missing physical SIM status");
  size_t offset=1;std::map<unsigned,Bytes> out;
  for(unsigned slot=0;slot<bytes[0];++slot) {
    if(bytes.size()-offset<10)throw std::runtime_error("Truncated physical SIM status");
    const unsigned present=U32(&bytes[offset]),active=U32(&bytes[offset+4]);
    const unsigned logical=bytes[offset+8],length=bytes[offset+9];offset+=10;
    if(length>bytes.size()-offset)throw std::runtime_error("Truncated SIM identifier");
    if(present==2&&active==1&&logical>0&&length>=5&&length<=20)
      out[logical]=Bytes(bytes.begin()+offset,bytes.begin()+offset+length);
    offset+=length;
  }
  return out;
}
}
