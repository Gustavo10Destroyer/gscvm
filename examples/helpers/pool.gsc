#include symbol;

create_pool( amount )
{
    if( !symbols_exists() || !isdefined( level.symbols.pool ) )
    {
        // Must follow this order
        pool = create_symbol( "Pool" );
        level.symbols.pool = pool;
    }

    this = spawnstruct();
    this.kind = level.symbols.pool;
    this.amount = amount;
    return this;
}