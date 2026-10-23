arraytest()
{
	a = [];
	a["x"] = 10;
	a[2] = 5;
	total = 0;
	foreach (key, value in a)
		total += value;
	return total;
}

stringtest()
{
	return "a" + "b" + 3;
}

vectest()
{
	v = (1, 2, 3);
	w = vectorscale(v, 2);
	return w[0] + w[1] + w[2];
}

addfield(x)
{
	self.v = x;
	return self.v + 1;
}

methodtest()
{
	return level addfield(41);
}

waitfunc()
{
	level waittill("ping", a, b);
	return a + b;
}

startwait()
{
	thread waitfunc();
	wait 0;
	level notify("ping", 3, 4);
	return 0;
}

endontest()
{
	level endon("stop");
	wait 100;
	return 1;
}

array_native()
{
	return array("item 1", "item 2", 3);
}

array_empty()
{
	return array();
}
