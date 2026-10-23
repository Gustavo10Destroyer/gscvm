main()
{
	println( "gscvm basics" );
	println( "add(20, 22) = &&1", add( 20, 22 ) );

	values = array( 3, 5, 8 );
	println( "sum = &&1", sum( values ) );

	point = ( 1, 2, 3 );
	println( "point = &&1", point );

	return add( 20, 22 );
}

add( a, b )
{
	return a + b;
}

sum( values )
{
	total = 0;
	foreach ( index, value in values )
		total += value;
	return total;
}
