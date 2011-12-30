/*
 * Copyright (c) 2010-2011 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "luainterface.h"
#include "luaobject.h"

#include <framework/core/resourcemanager.h>
#include <lua.hpp>

LuaInterface g_lua;

LuaInterface::LuaInterface() :
    L(NULL), m_weakTableRef(0)
{
}

LuaInterface::~LuaInterface()
{
}

void LuaInterface::init()
{
    createLuaState();

    // check if demangle_type is working as expected
    assert(Fw::demangleType<LuaObject>() == "LuaObject");

    // register LuaObject, the base of all other objects
    registerClass<LuaObject>();
    bindClassMemberGetField<LuaObject>("use_count", &LuaObject::getUseCount);
}

void LuaInterface::terminate()
{
    // close lua state, it will release all objects
    closeLuaState();
}

void LuaInterface::registerStaticClass(const std::string& className)
{
    newTable();
    pushValue();
    setGlobal(className);
    pop();
}

void LuaInterface::registerClass(const std::string& className, const std::string& baseClass)
{
    // creates the class table (that it's also the class methods table)
    newTable();
    pushValue();
    setGlobal(className);
    const int klass = getTop();

    // creates the class fieldmethods table
    newTable();
    pushValue();
    setGlobal(className + "_fieldmethods");
    int klass_fieldmethods = getTop();

    // creates the class metatable
    newTable();
    pushValue();
    setGlobal(className + "_mt");
    int klass_mt = getTop();

    // set metatable metamethods
    pushCppFunction(&LuaInterface::luaObjectGetEvent);
    setField("__index", klass_mt);
    pushCppFunction(&LuaInterface::luaObjectSetEvent);
    setField("__newindex", klass_mt);
    pushCppFunction(&LuaInterface::luaObjectEqualEvent);
    setField("__eq", klass_mt);
    pushCppFunction(&LuaInterface::luaObjectCollectEvent);
    setField("__gc", klass_mt);

    // set some fields that will be used later in metatable
    pushValue(klass);
    setField("methods", klass_mt);
    pushValue(klass_fieldmethods);
    setField("fieldmethods", klass_mt);

    // redirect methods and fieldmethods to the base class ones
    if(!className.empty() && className != "LuaObject") {
        // the following code is what create classes hierarchy for lua, by reproducing:
        // DerivedClass = { __index = BaseClass }
        // DerivedClass_fieldmethods = { __index = BaseClass_methods }

        // redirect the class methods to the base methods
        pushValue(klass);
        newTable();
        getGlobal(baseClass);
        setField("__index");
        setMetatable();
        pop();

        // redirect the class fieldmethods to the base fieldmethods
        pushValue(klass_fieldmethods);
        newTable();
        getGlobal(baseClass + "_fieldmethods");
        setField("__index");
        setMetatable();
        pop();
    }

    // pops klass, klass_mt, klass_fieldmethods
    pop(3);
}

void LuaInterface::registerClassStaticFunction(const std::string& className,
                                               const std::string& functionName,
                                               const LuaCppFunction& function)
{
    registerClassMemberFunction(className, functionName, function);
}

void LuaInterface::registerClassMemberFunction(const std::string& className,
                                               const std::string& functionName,
                                               const LuaCppFunction& function)
{
    getGlobal(className);
    pushCppFunction(function);
    setField(functionName);
    pop();
}

void LuaInterface::registerClassMemberField(const std::string& className,
                                            const std::string& field,
                                            const LuaCppFunction& getFunction,
                                            const LuaCppFunction& setFunction)
{
    getGlobal(className + "_fieldmethods");

    if(getFunction) {
        pushCppFunction(getFunction);
        setField(Fw::mkstr("get_", field));
    }

    if(setFunction) {
        pushCppFunction(setFunction);
        setField(Fw::mkstr("set_", field));
    }

    pop();
}

void LuaInterface::registerGlobalFunction(const std::string& functionName, const LuaCppFunction& function)
{
    pushCppFunction(function);
    setGlobal(functionName);
}

int LuaInterface::luaObjectGetEvent(LuaInterface* lua)
{
    // stack: obj, key
    LuaObjectPtr obj = lua->toObject(-2);
    std::string key = lua->toString(-1);
    assert(obj);

    lua->remove(-1); // removes key

    // if a get method for this key exists, calls it
    lua->getMetatable(); // pushes obj metatable
    lua->getField("fieldmethods"); // push obj fieldmethods
    lua->remove(-2); // removes obj metatable
    lua->getField("get_" + key); // pushes get method
    lua->remove(-2); // remove obj fieldmethods
    if(!lua->isNil()) { // is the get method not nil?
        lua->insert(-2); // moves obj to the top
        lua->protectedCall(1, 1); // calls get method, arguments: obj
        return 1;
    }
    lua->pop(); // pops the nil get method

    // if the field for this key exists, returns it
    obj->luaGetField(key);
    if(!lua->isNil()) {
        lua->remove(-2); // removes the obj
        // field value is on the stack
        return 1;
    }
    lua->pop(); // pops the nil field

    // pushes the method assigned by this key
    lua->getMetatable();  // pushes obj metatable
    lua->getField("methods"); // push obj methods
    lua->remove(-2); // removes obj metatable
    lua->getField(key); // pushes obj method
    lua->remove(-2); // remove obj methods
    lua->remove(-2); // removes obj

    // the result value is on the stack
    return 1;
}

int LuaInterface::luaObjectSetEvent(LuaInterface* lua)
{
    // stack: obj, key, value
    LuaObjectPtr obj = lua->toObject(-3);
    std::string key = lua->toString(-2);
    assert(obj);

    lua->remove(-2); // removes key
    lua->insert(-2); // moves obj to the top

    // check if a set method for this field exists and call it
    lua->getMetatable(); // pushes obj metatable
    lua->getField("fieldmethods"); // push obj fieldmethods
    lua->remove(-2); // removes obj metatable
    lua->getField("set_" + key); // pushes set method
    lua->remove(-2); // remove obj fieldmethods
    if(!lua->isNil()) { // is the set method not nil?
        lua->insert(-3); // moves func to -3
        lua->insert(-2); // moves obj to -2, and value to -1
        lua->protectedCall(2, 0); // calls set method, arguments: obj, value
        return 0;
    }
    lua->pop(); // pops the nil set method

    // no set method exists, then treats as an field and set it
    lua->pop(); // pops the object
    obj->luaSetField(key); // sets the obj field
    return 0;
}

int LuaInterface::luaObjectEqualEvent(LuaInterface* lua)
{
    // stack: obj1, obj2
    bool ret = false;

    // check if obj1 == obj2
    if(lua->isUserdata(-1) && lua->isUserdata(-2)) {
        LuaObjectPtr* objPtr2 = static_cast<LuaObjectPtr*>(lua->popUserdata());
        LuaObjectPtr* objPtr1 = static_cast<LuaObjectPtr*>(lua->popUserdata());
        assert(objPtr1 && objPtr2);
        if(*objPtr1 == *objPtr2)
            ret = true;
    } else
        lua->pop(2);

    lua->pushBoolean(ret);
    return 1;
}

int LuaInterface::luaObjectCollectEvent(LuaInterface* lua)
{
    // gets object pointer
    auto objPtr = static_cast<LuaObjectPtr*>(lua->popUserdata());
    assert(objPtr);

    // resets pointer to decrease object use count
    objPtr->reset();
    return 0;
}


///////////////////////////////////////////////////////////////////////////////


void LuaInterface::runScript(const std::string& fileName)
{
    loadScript(fileName);
    safeCall();
}

void LuaInterface::runBuffer(const std::string& buffer, const std::string& source)
{
    loadBuffer(buffer, source);
    safeCall();
}

void LuaInterface::loadScript(const std::string& fileName)
{
    std::string buffer = g_resources.loadFile(fileName);
    std::string source = "@" + g_resources.resolvePath(fileName);
    loadBuffer(g_resources.loadFile(fileName), "@" + g_resources.resolvePath(fileName));
}

void LuaInterface::loadFunction(const std::string& buffer, const std::string& source)
{
    // gets the function contained in that buffer
    if(boost::starts_with(buffer, "function")) {
        // evaluate the function
        std::string buf = Fw::mkstr("__func = ", buffer);
        loadBuffer(buf, source);
        safeCall();

        // get the function
        getGlobal("__func");

        // reset the global __func
        pushNil();
        setGlobal("__func");
    }
    // gets the buffer as a function
    else if(!buffer.empty())
        loadBuffer(buffer, source);
    else
        pushNil();
}

void LuaInterface::evaluateExpression(const std::string& expression, const std::string& source)
{
    // evaluates the expression
    if(!expression.empty()) {
        std::string buffer = Fw::mkstr("__exp = (", expression, ")");
        loadBuffer(buffer, source);
        safeCall();

        // gets the expression result
        getGlobal("__exp");

        // resets global __exp
        pushNil();
        setGlobal("__exp");
    } else
        pushNil();
}

std::string LuaInterface::traceback(const std::string& errorMessage, int level)
{
    // gets debug.traceback
    getGlobal("debug");
    getField("traceback");
    remove(-2); // remove debug

    // calls debug.traceback(errorMessage, level)
    pushString(errorMessage);
    pushInteger(level);
    call(2,1);

    // returns the traceback message
    return popString();
}

std::string LuaInterface::getCurrentSourcePath(int level)
{
    std::string path;
    if(!L)
        return path;

    // check all stack functions for script source path
    while(true) {
        getStackFunction(level); // pushes stack function

        // only lua functions is wanted, because only them have a working directory
        if(isLuaFunction()) {
            path = functionSourcePath();
            break;
        } else if(isNil()) {
            pop();
            break;
        } else
            pop();

        // next level
        level++;
    }

    return path;
}

int LuaInterface::safeCall(int numArgs)
{
    assert(hasIndex(-numArgs-1));

    // saves the current stack size for calculating the number of results later
    int previousStackSize = stackSize();

    // pushes error function
    int errorFuncIndex = previousStackSize - numArgs;
    pushCFunction(&LuaInterface::luaErrorHandler);
    insert(errorFuncIndex);

    // calls the function in protected mode (means errors will be caught)
    int ret = pcall(numArgs, LUA_MULTRET, errorFuncIndex);

    remove(errorFuncIndex); // remove error func

     // if there was an error throw an exception
    if(ret != 0)
        throw LuaException(popString());

    // returns the number of results
    return (stackSize() + numArgs + 1) - previousStackSize;
}

int LuaInterface::protectedCall(int numArgs, int requestedResults)
{
    int numRets = 0;
    int funcIndex = -numArgs-1;

    try {
        // must be a function
        if(isFunction(funcIndex)) {
            numRets = safeCall(numArgs);

            if(requestedResults != -1) {
                if(numRets != requestedResults)
                    throw LuaException("function call didn't return the expected number of results", 0);
            }
        }
        // can also calls table of functions
        else if(isTable(funcIndex)) {
            // loop through table values
            pushNil();
            bool done = false;
            while(next(funcIndex-1)) {
                if(isFunction()) {
                    // repush arguments
                    for(int i=0;i<numArgs;++i)
                        pushValue(-numArgs-2);

                    int rets = safeCall(numArgs);
                    if(rets == 1) {
                        done = popBoolean();
                        if(done) {
                            pop();
                            break;
                        }
                    } else if(rets != 0)
                        throw LuaException("function call didn't return the expected number of results", 0);
                } else {
                    throw LuaException("attempt to call a non function", 0);
                }
            }
            pop(numArgs + 1); // pops the table of function and arguments

            if(requestedResults == -1 || requestedResults == 1) {
                numRets = 1;
                pushBoolean(done);
            }
        }
        // nil values are ignored
        else if(isNil(funcIndex)) {
            pop(numArgs + 1); // pops the function and arguments
        }
        // if not nil, warn
        else {
            throw LuaException("attempt to call a non function value", 0);
        }
    } catch(LuaException &e) {
        logError("protected lua call failed: ", e.what());
    }

    // pushes nil values if needed
    while(requestedResults != -1 && numRets < requestedResults) {
        pushNil();
        numRets++;
    }

    // returns the number of results on the stack
    return numRets;
}

void LuaInterface::newEnvironment()
{
    newTable(); // pushes the new environment table
    newTable(); // pushes the new environment metatable
    getGlobalEnvironment();  // pushes the global environment
    setField("__index"); // sets metatable __index to the global environment
    setMetatable(); // assigns environment metatable
}


///////////////////////////////////////////////////////////////////////////////
// lua C functions

int LuaInterface::luaScriptLoader(lua_State* L)
{
    // loads the script as a function
    std::string fileName = g_lua.popString();
    fileName += ".lua";
    try {
        g_lua.loadScript(fileName);
        return 1;
    } catch(LuaException& e) {
        logError("failed to load script file '", fileName, "' :'", e.what());
        return 0;
    }
}

int LuaInterface::luaErrorHandler(lua_State* L)
{
    // pops the error message
    auto error = g_lua.popString();

    // prevents repeated tracebacks
    if(error.find("stack traceback:") == std::string::npos)
        error = g_lua.traceback(error, 1);

    // pushes the new error message with traceback information
    g_lua.pushString(error);
    return 1;
}

int LuaInterface::luaCppFunctionCallback(lua_State* L)
{
    // retrieves function pointer from userdata
    auto funcPtr = static_cast<LuaCppFunctionPtr*>(g_lua.popUpvalueUserdata());
    assert(funcPtr);

    int numRets = 0;

    // do the call
    try {
        numRets = (*(funcPtr->get()))(&g_lua);
        assert(numRets == g_lua.stackSize());
    } catch(LuaException &e) {
        logError("lua cpp callback failed: ", e.what());
    }

    return numRets;
}

int LuaInterface::luaCollectCppFunction(lua_State* L)
{
    auto funcPtr = static_cast<LuaCppFunctionPtr*>(g_lua.popUserdata());
    assert(funcPtr);
    funcPtr->reset();
    return 0;
}


///////////////////////////////////////////////////////////////////////////////
// from here all next functions are interfaces for the Lua API

void LuaInterface::createLuaState()
{
    // creates lua state
    L = luaL_newstate();
    if(!L)
        logFatal("Unable to create lua state");

    // load lua standard libraries
    luaL_openlibs(L);

    // creates weak table
    newTable();
    newTable();
    pushString("v");
    setField("__mode");
    setMetatable();
    m_weakTableRef = ref();

    // installs script loader
    getGlobal("package");
    getField("loaders");
    pushCFunction(&LuaInterface::luaScriptLoader);
    rawSeti(5);
    pop(2);
}

void LuaInterface::closeLuaState()
{
    if(L) {
        // close lua, it also collects
        lua_close(L);
        L = NULL;
    }
}

void LuaInterface::collectGarbage()
{
    // prevents recursive collects
    static bool collecting = false;
    if(!collecting) {
        collecting = true;

        // we must collect two times because __gc metamethod
        // is called on uservalues only the second time
        for(int i=0;i<2;++i)
            lua_gc(L, LUA_GCCOLLECT, 0);

        collecting = false;
    }
}

void LuaInterface::loadBuffer(const std::string& buffer, const std::string& source)
{
    // loads lua buffer
    int ret = luaL_loadbuffer(L, buffer.c_str(), buffer.length(), source.c_str());
    if(ret != 0)
        throw LuaException(popString(), 0);
}

int LuaInterface::pcall(int numArgs, int numRets, int errorFuncIndex)
{
    assert(hasIndex(-numArgs - 1));
    return lua_pcall(L, numArgs, numRets, errorFuncIndex);
}

void LuaInterface::call(int numArgs, int numRets)
{
    assert(hasIndex(-numArgs - 1));
    lua_call(L, numArgs, numRets);
}

void LuaInterface::throwError()
{
    assert(hasIndex(-1));
    lua_error(L);
}

int LuaInterface::ref()
{
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

int LuaInterface::weakRef()
{
    static int id = 0;

    // generates a new id
    ++id;
    if(id == 2147483647)
        id = 0;

    // gets weak table
    getRef(m_weakTableRef);
    insert(-2);

    // sets weak_table[id] = v
    rawSeti(id);

    // pops weak table
    pop();

    return id;
}

void LuaInterface::unref(int ref)
{
    if(ref >= 0 && L != NULL)
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

const char* LuaInterface::typeName(int index)
{
    assert(hasIndex(index));
    int type = lua_type(L, index);
    return lua_typename(L, type);
}

std::string LuaInterface::functionSourcePath()
{
    std::string path;

    // gets function source path
    lua_Debug ar;
    memset(&ar, 0, sizeof(ar));
    lua_getinfo(L, ">Sn", &ar);
    if(ar.source) {
        // scripts coming from files has source beginning with '@'
        if(ar.source[0] == '@') {
            path = ar.source;
            path = path.substr(1, path.find_last_of("/") - 1);
            path = path.substr(0, path.find_last_of(":"));
        }
    }

    return path;
}

void LuaInterface::insert(int index)
{
    assert(hasIndex(index));
    lua_insert(L, index);
}

void LuaInterface::remove(int index)
{
    assert(hasIndex(index));
    lua_remove(L, index);
}

bool LuaInterface::next(int index)
{
    assert(hasIndex(index));
    return lua_next(L, index);
}

void LuaInterface::getStackFunction(int level)
{
    lua_Debug ar;
    if(lua_getstack(L, level, &ar) == 1)
        lua_getinfo(L, "f", &ar);
    else
        pushNil();
}

void LuaInterface::getRef(int ref)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    //logTraceDebug(ref);
}

void LuaInterface::getWeakRef(int weakRef)
{
    // pushes weak_table[weakRef]
    getRef(m_weakTableRef);
    rawGeti(weakRef);
    remove(-2);
}

void LuaInterface::getGlobalEnvironment()
{
    pushThread();
    getEnv();
    remove(-2);
}

void LuaInterface::setGlobalEnvironment()
{
    pushThread();
    insert(-2);
    setEnv();
}

void LuaInterface::setMetatable(int index)
{
    assert(hasIndex(index));
    lua_setmetatable(L, index);
}

void LuaInterface::getMetatable(int index)
{
    assert(hasIndex(index));
    lua_getmetatable(L, index);
}

void LuaInterface::getField(const char* key, int index)
{
    assert(hasIndex(index));
    assert(isUserdata(index) || isTable(index));
    lua_getfield(L, index, key);
}

void LuaInterface::setField(const char* key, int index)
{
    assert(hasIndex(index));
    assert(isUserdata(index) || isTable(index));
    lua_setfield(L, index, key);
}

void LuaInterface::getEnv(int index)
{
    assert(hasIndex(index));
    lua_getfenv(L, index);
}

void LuaInterface::setEnv(int index)
{
    assert(hasIndex(index));
    assert(lua_setfenv(L, index) == 1);
}

void LuaInterface::getTable(int index)
{
    assert(hasIndex(index));
    lua_gettable(L, index);
}

void LuaInterface::setTable(int index)
{
    assert(hasIndex(index));
    lua_settable(L, index);
}

void LuaInterface::getGlobal(const std::string& key)
{
    lua_getglobal(L, key.c_str());
}

void LuaInterface::getGlobalField(const std::string& globalKey, const std::string& fieldKey)
{
    getGlobal(globalKey);
    if(!isNil()) {
        assert(isTable() || isUserdata());
        getField(fieldKey);
        remove(-2);
    }
}

void LuaInterface::setGlobal(const std::string& key)
{
    assert(hasIndex(-1));
    lua_setglobal(L, key.c_str());
}

void LuaInterface::rawGet(int index)
{
    assert(hasIndex(index));
    lua_rawget(L, index);
}

void LuaInterface::rawGeti(int n, int index)
{
    assert(hasIndex(index));
    lua_rawgeti(L, index, n);
}

void LuaInterface::rawSet(int index)
{
    assert(hasIndex(index));
    lua_rawset(L, index);
}

void LuaInterface::rawSeti(int n, int index)
{
    assert(hasIndex(index));
    lua_rawseti(L, index, n);
}

void LuaInterface::newTable()
{
    lua_newtable(L);
}

void* LuaInterface::newUserdata(int size)
{
    return lua_newuserdata(L, size);
}

void LuaInterface::pop(int n)
{
    if(n > 0) {
        assert(hasIndex(-n));
        lua_pop(L, n);
    }
}

int LuaInterface::popInteger()
{
    assert(hasIndex(-1));
    int v = toInteger(-1);
    pop();
    return v;
}

double LuaInterface::popNumber()
{
    assert(hasIndex(-1));
    double v = toNumber(-1);
    pop();
    return v;
}

bool LuaInterface::popBoolean()
{
    assert(hasIndex(-1));
    bool v = toBoolean(-1);
    pop();
    return v;
}

std::string LuaInterface::popString()
{
    assert(hasIndex(-1));
    std::string v = toString(-1);
    pop();
    return v;
}

void* LuaInterface::popUserdata()
{
    assert(hasIndex(-1));
    void* v = toUserdata(-1);
    pop();
    return v;
}

LuaObjectPtr LuaInterface::popObject()
{
    assert(hasIndex(-1));
    LuaObjectPtr v = toObject(-1);
    pop();
    return v;
}

void* LuaInterface::popUpvalueUserdata()
{
    return lua_touserdata(L, lua_upvalueindex(1));
}

void LuaInterface::pushNil()
{
    lua_pushnil(L);
}

void LuaInterface::pushInteger(int v)
{
    lua_pushinteger(L, v);
}

void LuaInterface::pushNumber(double v)
{
    lua_pushnumber(L, v);
}

void LuaInterface::pushBoolean(bool v)
{
    lua_pushboolean(L, v);
}

void LuaInterface::pushCString(const char* v)
{
    lua_pushstring(L, v);
}

void LuaInterface::pushLightUserdata(void* p)
{
    lua_pushlightuserdata(L, p);
}

void LuaInterface::pushThread()
{
    lua_pushthread(L);
}

void LuaInterface::pushObject(const LuaObjectPtr& obj)
{
    // fills a new userdata with a new LuaObjectPtr pointer
    new(newUserdata(sizeof(LuaObjectPtr))) LuaObjectPtr(obj);

    // set the userdata metatable
    getGlobal(Fw::mkstr(obj->getLuaObjectName(), "_mt"));
    assert(!isNil());
    setMetatable();
}

void LuaInterface::pushCFunction(LuaCFunction func, int n)
{
    lua_pushcclosure(L, func, n);
}

void LuaInterface::pushCppFunction(const LuaCppFunction& func)
{
    // create a pointer to func (this pointer will hold the function existence)
    new(newUserdata(sizeof(LuaCppFunctionPtr))) LuaCppFunctionPtr(new LuaCppFunction(func));

    // sets the userdata __gc metamethod, needed to free the function pointer when it gets collected
    newTable();
    pushCFunction(&LuaInterface::luaCollectCppFunction);
    setField("__gc");
    setMetatable();

    // actually pushes a C function callback that will call the cpp function
    pushCFunction(&LuaInterface::luaCppFunctionCallback, 1);
}

void LuaInterface::pushValue(int index)
{
    assert(hasIndex(index));
    lua_pushvalue(L, index);
}

bool LuaInterface::isNil(int index)
{
    assert(hasIndex(index));
    return lua_isnil(L, index);
}

bool LuaInterface::isBoolean(int index)
{
    assert(hasIndex(index));
    return lua_isboolean(L, index);
}

bool LuaInterface::isNumber(int index)
{
    assert(hasIndex(index));
    return lua_isnumber(L, index);
}

bool LuaInterface::isString(int index)
{
    assert(hasIndex(index));
    return lua_isstring(L, index);
}

bool LuaInterface::isTable(int index)
{
    assert(hasIndex(index));
    return lua_istable(L, index);
}

bool LuaInterface::isFunction(int index)
{
    assert(hasIndex(index));
    return lua_isfunction(L, index);
}

bool LuaInterface::isCFunction(int index)
{
    assert(hasIndex(index));
    return lua_iscfunction(L, index);
}

bool LuaInterface::isUserdata(int index)
{
    assert(hasIndex(index));
    return lua_isuserdata(L, index);
}

bool LuaInterface::toBoolean(int index)
{
    assert(hasIndex(index));
    return (bool)lua_toboolean(L, index);
}

int LuaInterface::toInteger(int index)
{
    assert(hasIndex(index));
    return lua_tointeger(L, index);
}

double LuaInterface::toNumber(int index)
{
    assert(hasIndex(index));
    return lua_tonumber(L, index);
}

const char* LuaInterface::toCString(int index)
{
    assert(hasIndex(index));
    return lua_tostring(L, index);
}

std::string LuaInterface::toString(int index)
{
    assert(hasIndex(index));
    std::string str;
    const char *c_str = lua_tostring(L, index);
    if(c_str)
        str = c_str;
    return str;
}

void* LuaInterface::toUserdata(int index)
{
    assert(hasIndex(index));
    return lua_touserdata(L, index);
}

LuaObjectPtr LuaInterface::toObject(int index)
{
    assert(hasIndex(index));
    if(isUserdata(index)) {
        LuaObjectPtr* objRef = static_cast<LuaObjectPtr*>(toUserdata(index));
        if(objRef && *objRef)
            return *objRef;
    }
    return nullptr;
}

int LuaInterface::getTop()
{
    return lua_gettop(L);
}

