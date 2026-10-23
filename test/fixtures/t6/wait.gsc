waiter()
{
	level waittill("ping", a, b);
	return a + b;
}

starter()
{
	thread waiter();
	return 0;
}

sleeper()
{
	wait 0.5;
	return 7;
}

ender()
{
	level endon("stop");
	wait 100;
	return 99;
}

matcher()
{
	level waittillmatch("ping", 3);
	return 1;
}
