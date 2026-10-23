main()
{
	println("hello");
	println(1 + 2);
	return add(3, 4);
}

add(a, b)
{
	return a + b;
}

fact(n)
{
	if (n <= 1)
		return 1;
	return n * fact(n - 1);
}

loopsum(n)
{
	total = 0;
	for (i = 0; i < n; i++)
		total += i;
	return total;
}

switchtest(x)
{
	switch (x)
	{
		case 1:
			return "one";
		case 2:
			return "two";
		default:
			return "other";
	}
}

nativecall(x)
{
	return test_echo(x);
}

methodnative()
{
	return level test_self();
}

divzero()
{
	return 1 / 0;
}
