// SPDX-License-Identifier: Apache-2.0
#pragma once

void tuningTests(const char* wujiXml, const char* wujiOverlay, const char* baldurXml, const char* baldurOverlay,
                 const char* tempDir);
void recorderTests(const char* tempDir);
void archiveTests(const char*, const char*, const char*, const char*, const char*);
int archiveProtocol(const char* stock, const char* overlay, const char* directory);
#ifdef GAMEPOLICY_IO_FAULT_TEST
void archiveIoFault(bool failOpen, bool failSync);
namespace gamepolicy { class Policy; bool archiveRoundTrip(const Policy&); }
#endif
void analysisTests(const char* baldurXml, const char* baldurOverlay);
