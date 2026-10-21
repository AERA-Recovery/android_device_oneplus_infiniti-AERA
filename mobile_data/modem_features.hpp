/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "qmi.hpp"
#include <tinyxml2.h>
#include <fcntl.h>
#include <sys/stat.h>

namespace aera::mobile {
inline std::string ParseModemFeatures(const Bytes &record) {
  const auto end=std::find(record.begin(),record.end(),uint8_t(0));
  if(end==record.end())throw std::runtime_error("Modem feature record is not terminated");
  const size_t length=end-record.begin();tinyxml2::XMLDocument xml;
  if(xml.Parse(reinterpret_cast<const char *>(record.data()),length)!=tinyxml2::XML_SUCCESS)
    throw std::runtime_error("Modem feature record is invalid");
  const auto *root=xml.RootElement();
  if(!root||std::string(root->Name())!="Feature"||root->NextSiblingElement())
    throw std::runtime_error("Unexpected modem feature record");
  const auto text=[&](const char *name) {
    const auto *element=root->FirstChildElement(name);
    if(!element||element->FirstChildElement()||element->NextSiblingElement(name)||!element->GetText())
      throw std::runtime_error("Modem feature record is incomplete");
    return std::string(element->GetText());
  };
  const auto id=text("emmcid"),svn=text("imeiSv");
  if(text("mdmfeature")!="true"||id.empty()||id.size()>32||
      id.find_first_not_of("0123456789")!=std::string::npos||svn.empty()||svn.size()>2||
      svn.find_first_not_of("0123456789")!=std::string::npos)
    throw std::runtime_error("Modem feature record is incomplete");
  return std::string(record.begin(),end);
}

inline void LoadModemFeatures() {
  Fd source(open("/dev/block/bootdevice/by-name/oplusreserve1",O_RDONLY|O_CLOEXEC));
  struct stat partition{};
  if(source.get()<0||fstat(source.get(),&partition)||!S_ISBLK(partition.st_mode))
    throw std::runtime_error("Cannot read the device modem feature record");
  // Match the SM8850 OEM feature-map layout, without running its NV-writing tool.
  Bytes record(12288);
  const off_t offset=access("/dev/block/sda",F_OK)==0?0x493000:0x46a000;
  if(pread(source.get(),record.data(),record.size(),offset)!=static_cast<ssize_t>(record.size()))
    throw std::runtime_error("Cannot read the device modem feature record");
  const auto xml=ParseModemFeatures(record);
  Fd target(open("/dev/mdmfeature",O_WRONLY|O_CLOEXEC|O_NOFOLLOW));struct stat device{};
  if(target.get()<0||fstat(target.get(),&device)||!S_ISCHR(device.st_mode))
    throw std::runtime_error("Modem feature-map interface is unavailable");
  // This character driver copies the existing record into shared RAM only.
  if(write(target.get(),xml.c_str(),xml.size()+1)!=static_cast<ssize_t>(xml.size()+1))
    throw std::runtime_error("Cannot initialize the modem feature map");
}
}
