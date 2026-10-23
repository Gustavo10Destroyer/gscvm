vector_string()
{
	v = (1, 2, 3);
	return "v=" + v;
}

struct_test()
{
	s = spawnstruct();
	s.method = "GET";

	headers = [];
	headers["user-agent"] = "GSCVM/beta";
	s.headers = headers;

	s.nested = spawnstruct();
	s.nested.value = 9;
	s.headers["accept"] = "text/plain";

	return s.method + "|" + s.headers["user-agent"] + "|" + s.nested.value + "|" + s.headers["accept"];
}

level_debug()
{
	level.test_value = 5;
	return level;
}
