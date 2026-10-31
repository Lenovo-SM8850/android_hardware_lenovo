// SPDX-License-Identifier: Apache-2.0
#pragma once

void tuningTests(const char* wujiXml, const char* wujiOverlay, const char* baldurXml, const char* baldurOverlay,
                 const char* tempDir);
void recorderTests(const char* tempDir);
void analysisTests(const char* baldurXml, const char* baldurOverlay);
