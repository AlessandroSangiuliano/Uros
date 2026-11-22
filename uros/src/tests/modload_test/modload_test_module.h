/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * modload_test_module.h -- what modload_test's modules give it (#973).
 *
 * Each module is <basename>_modload_test_ops in its file, and says which
 * pool it was built for, under which file name, and answers from its own
 * code: a module that came from the wrong pool says so in all three.
 */

#ifndef _MODLOAD_TEST_MODULE_H_
#define _MODLOAD_TEST_MODULE_H_

#define MODLOAD_TEST_SUFFIX	"_modload_test_ops"

struct modload_test_ops {
	const char	*class;		/* the pool it was built for */
	const char	*file;		/* its file name in that pool */
	int		(*answer)(void);	/* a letter only its own code gives */
};

#endif /* _MODLOAD_TEST_MODULE_H_ */
