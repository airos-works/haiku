/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef TERMINAL_LINKS_H
#define TERMINAL_LINKS_H


#include <string>
#include <vector>

#include "Terminal.h"


struct TermLinkSegment {
	TermPoint	start;
	TermPoint	end;		// exclusive, on the same row
};


struct TermLinkRange {
	TermPoint	start;		// first cell
	TermPoint	end;		// after the last cell
	std::vector<TermLinkSegment> segments;
							// the cells of the link, one run per row
	std::string	uri;
	uint32		hyperlink;	// the OSC 8 link id, 0 for an address found
							// in the text
	std::string	identifier;	// the OSC 8 "id" parameter
};


//!	The link at \a position: an OSC 8 hyperlink, or an address in the text,
//!	followed over the lines a program or the terminal broke it into. The
//!	terminal must be locked.
bool	FindTerminalLink(const Terminal& terminal, TermPoint position,
			TermLinkRange& _link);


#endif	// TERMINAL_LINKS_H
