/*
 * dlopen_exit_test [rounds]: exit() while another thread is opening a
 * library with many dependencies. terminate_program() sorted the images
 * without the loader's lock and crashed on one whose dependencies were still
 * being set (a web process ending while Mesa loaded, summit#119). Each round
 * is a child process; the test fails when one of them does not exit with 0.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>


static void*
open_libraries(void* data)
{
	(void)data;
	for (;;) {
		void* handle = dlopen("libtracker.so", RTLD_NOW);
		if (handle != NULL)
			dlclose(handle);
	}
	return NULL;
}


int
main(int argc, char** argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 200;
	int failed = 0;
	for (int i = 0; i < rounds; i++) {
		pid_t child = fork();
		if (child == 0) {
			pthread_t thread;
			pthread_create(&thread, NULL, open_libraries, NULL);
			snooze(100 + (i * 997) % 20000);
			exit(0);
		}
		int status = 0;
		waitpid(child, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			failed++;
			printf("round %d: child status %#x\n", i, status);
		}
	}
	printf("%d of %d children did not exit cleanly\n", failed, rounds);
	return failed != 0;
}
