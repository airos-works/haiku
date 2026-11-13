/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NET_COALESCE_H
#define NET_COALESCE_H


struct list;

void coalesce_tcp_segments(struct list* batch);


#endif	// NET_COALESCE_H
