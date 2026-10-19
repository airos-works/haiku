#
# Shell integration for Terminal: bash marks where its prompts, the commands
# typed and their output begin (OSC 133), and tells the terminal its working
# directory (OSC 7), the way kitty's shell integration does. Terminal jumps
# from prompt to prompt with Ctrl+Shift+Z and Ctrl+Shift+X.
#
# Only in interactive bash shells that run in Terminal (TERM_PROGRAM is
# airos-terminal). AIROS_TERMINAL_INTEGRATION=0 in the environment turns it
# off.
#
# Copyright 2026, Haiku, Inc. All rights reserved.
# Distributed under the terms of the MIT License.
#

if [ -n "$BASH_VERSION" ] && [ "$TERM_PROGRAM" = "airos-terminal" ] \
		&& [ "$AIROS_TERMINAL_INTEGRATION" != 0 ]; then
	case $- in
	*i*)
		_airos_terminal_prompt()
		{
			local status=$?
			local directory=${PWD//%/%25}
			directory=${directory// /%20}

			# the previous command is done; where the shell is now
			printf '\033]133;D;%s\007\033]7;file://%s%s\007' "$status" \
				"${HOSTNAME:-localhost}" "$directory"

			# marks around the prompts, again if a profile replaced them
			case $PS1 in
			*'133;A'*)
				;;
			*)
				PS1='\[\033]133;A\007\]'"$PS1"'\[\033]133;B\007\]'
				;;
			esac
			case $PS2 in
			*'133;A'*)
				;;
			*)
				PS2='\[\033]133;A;k=s\007\]'"$PS2"'\[\033]133;B\007\]'
				;;
			esac
			return $status
		}

		PROMPT_COMMAND="_airos_terminal_prompt${PROMPT_COMMAND:+;$PROMPT_COMMAND}"
		# a command was entered: its output starts here
		PS0='\033]133;C\007'"$PS0"
		;;
	esac
fi
