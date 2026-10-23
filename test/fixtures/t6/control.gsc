strswitch(x)
{
	switch (x)
	{
		case "a": return 1;
		case "b": return 2;
		default: return 3;
	}
}

nested()
{
	a = [];
	a["in"] = [];
	a["in"][0] = 7;
	return a["in"][0];
}

whiletest(n)
{
	i = 0;
	total = 0;
	while (i < n)
	{
		total += i;
		i++;
	}
	return total;
}

dowhiletest(n)
{
	i = 0;
	do
	{
		i++;
	} while (i < n);
	return i;
}

bitops()
{
	return ((5 | 2) ^ 1) & 7;
}

boolops()
{
	x = true;
	y = false;
	if (x && !y)
		return 1;
	return 0;
}

ternary(x)
{
	return x > 0 ? 10 : 20;
}

sizetest()
{
	a = [];
	a[0] = 1;
	a[1] = 2;
	return a.size;
}

undeftest()
{
	a = undefined;
	a = 5;
	return isdefined(a);
}

cleartest()
{
	a = [];
	a["x"] = 1;
	a["y"] = 2;
	a["x"] = undefined;
	return a.size;
}

mathtest()
{
	return 7 / 2 + 7 % 4 + abs(-5);
}

angles()
{
	v = anglestoforward((0, 90, 0));
	return v[0] + v[1] + v[2];
}
