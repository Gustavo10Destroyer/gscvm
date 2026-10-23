create_coroutine( func, endons )
{
    this = spawnstruct();
    this.func = func;
    this.endons = endons;
    this.running = false;
    return this;
}

_coroutine_wrapper( coroutine )
{
    coroutine endon( "coroutine:stop" );

    for( i = 0; i < coroutine.endons.size; i++ )
        level endon( coroutine.endons[i] );

    [[ coroutine.func ]]();
}

start_coroutine( coroutine )
{
    if( coroutine.running )
    {
        assertmsg( "coroutine already running" );
        return;
    }

    coroutine.running = true;
    self thread _coroutine_wrapper( coroutine );
}

stop_coroutine( coroutine )
{
    if( !coroutine.running )
        return;

    coroutine notify( "coroutine:stop" );
    coroutine.running = false;
}