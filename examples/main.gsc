#include helpers\coroutine;

end_game()
{
    level notify( "end_game" );
    println( "^3end_game^7 emitted!" );
}

my_counter()
{
    for( i = 0; i < 10; i++ )
    {
        println( i );
        wait 1;
    }
}

main()
{
    coroutine = create_coroutine( ::my_counter, array( "end_game" ) );
    start_coroutine( coroutine );
    wait 5;
    end_game();
}