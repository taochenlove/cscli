/*************************************************************************
	> File Name: thread_dm.c
	> Author: 
	> Mail: 
	> Created Time: 2019年04月03日 星期三 18时14分07秒
 ************************************************************************/

#include<stdio.h>

#include "thread.h"

int test_timer(struct thread *thread)
{
	struct thread_master *master = (struct thread_master *)thread->arg;
	thread_add_timer(master, test_timer, (void *)master, 1);
	printf("======>>timer<<======\n");
	return 0;
}
int main()
{
	struct thread_master *master = thread_master_create ();
	struct thread thread;

	thread_add_timer(master, test_timer, (void *)master, 1);

	while (thread_fetch (master, &thread))
    	thread_call(&thread);

}
