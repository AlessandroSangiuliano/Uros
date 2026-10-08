/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A function's MSI-X enable, raced from two processors (#598's C10, -V).
 */

#ifndef _X86_64_CPU_MSI_RACE_TEST_H_
#define _X86_64_CPU_MSI_RACE_TEST_H_

void msi_function_race_test(void);

#endif	/* _X86_64_CPU_MSI_RACE_TEST_H_ */
