thread_main()
{
	level.waiter_thread = thread waiter();
	level.notifier_thread = thread notifier();
	level.ender_thread = thread ender();
	return 42;
}

waiter()
{
	level waittill("ping", a, b);
	level.waiter_result = a * 10 + b;
	return a + b;
}

notifier()
{
	wait 0.1;
	level notify("ping", 3, 4);
	return 1;
}

ender()
{
	level endon("ping");
	wait 100;
	return 99;
}

recursion(n)
{
	return recursion(n + 1);
}

spin()
{
	while (true)
	{
	}
}

combined()
{
	level endon("ping");
	level waittill("ping");
	return 1;
}

wait_fraction()
{
	wait 0.05;
	return 5;
}

wait_sequence()
{
	wait 1;
	wait(2);
	wait 0.05;
	return 7;
}

quick()
{
	return 9;
}

wait_tenth()
{
	wait 0.1;
	return 1;
}

wait_short()
{
	wait 0.01;
	return 3;
}

wait_frame()
{
	wait 0;
	return 2;
}

spawn_once()
{
	level.once_starts = 0;
	level.once_steps = 0;

	for (i = 0; i < 10; i++)
		thread once();
}

once()
{
	level notify("once:stop");
	level endon("once:stop");

	level.once_starts = level.once_starts + 1;

	for (i = 0; i < 3; i++)
	{
		level.once_steps = level.once_steps + 1;
		wait 1;
	}
}

sync_wrapper()
{
	level.sync_steps = 0;
	level.sync_wrapper_after = 0;
	level endon("sync:stop");
	sync_invoke(::sync_helper);
	level.sync_wrapper_after = 1;
}

sync_invoke(func)
{
	[[func]]();
}

sync_helper()
{
	level.sync_steps = level.sync_steps + 1;
	wait 1;
	level.sync_steps = level.sync_steps + 1;
	wait 1;
	level.sync_steps = level.sync_steps + 1;
}

sync_self_wrapper()
{
	level.sync_self_after = 0;
	level.sync_self_notified = 0;
	level endon("sync:self");
	sync_self_invoke();
	level.sync_self_after = 1;
}

sync_self_invoke()
{
	level notify("sync:self");
	level.sync_self_notified = 1;
}
