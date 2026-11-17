/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef VC4_FIELDS_H
#define VC4_FIELDS_H


/*	Bit field helpers in the form Linux' vc4 sources use them: a field FOO
	has FOO_MASK and FOO_SHIFT. */


#include <SupportDefs.h>


#ifndef BIT
#	define BIT(bit)				((uint32)1 << (bit))
#endif

#define VC4_MASK(high, low) \
	((uint32)((((uint64)1 << ((high) - (low) + 1)) - 1) << (low)))

#define VC4_SET_FIELD(value, field) \
	(((uint32)(value) << field ## _SHIFT) & field ## _MASK)

#define VC4_GET_FIELD(word, field) \
	(((uint32)(word) & field ## _MASK) >> field ## _SHIFT)


#endif	/* VC4_FIELDS_H */
