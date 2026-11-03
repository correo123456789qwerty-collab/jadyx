// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include "graphics-frontend-fixture.h"
int main() {
    CheckEnvironmentRecord();
    std::cout << "PASS recorded shader inputs, replay side effects, mutations and missing data\n";
}
