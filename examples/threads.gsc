main()
{
	level.worker_result = 0;

	thread worker();

	// The runner advances logical time automatically for call()/run_until_complete().
	wait 0.05;
	level notify( "work", 6, 7 );
	wait 0.05;

	println( "worker result = &&1", level.worker_result );
	return level.worker_result;
}

worker()
{
	level waittill( "work", a, b );
	level.worker_result = a * b;
}
