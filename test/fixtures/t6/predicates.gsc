array_value()
{
	a = [];
	return isarray(a);
}

array_nonarray()
{
	return isarray("x");
}

string_value()
{
	return isstring("x");
}

string_nonstring()
{
	return isstring(1);
}

int_value()
{
	return isint(3);
}

int_nonint()
{
	return isint(3.5);
}

float_value()
{
	return isfloat(3.5);
}

float_nonfloat()
{
	return isfloat(3);
}

size_property()
{
	a = [];
	a[0] = 1;
	a[1] = 2;
	return a.size;
}

field_read()
{
	a = [];
	a["foo"] = 3;
	return a.foo;
}

field_clear()
{
	a = [];
	a["foo"] = 3;
	a.foo = undefined;
	return 1;
}
