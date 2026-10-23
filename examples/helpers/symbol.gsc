create_symbol( name )
{
    if( !isdefined( level.symbols ) )
    {
        level.symbols = spawnstruct();
        level.symbols.kind = "Symbol";
        level.symbols.name = "Symbols";
    }

    this = spawnstruct();
    this.name = name;
    this.kind = level.symbols;
    return this;
}

is_symbol( variable )
{
    return isdefined( variable )
        && symbols_exists()
        && isdefined( variable.kind )
        && variable.kind == level.symbols;
}

symbols_exists()
{
    return isdefined( level.symbols );
}