// The script runtime written in Luau itself: script instances, hooks, coroutines, events. The C++ host only loads files and pumps
// these functions once per frame. Included as a raw string by luau_host.cpp.
R"LUAU(
local tbd = tbd
local instances = {}   -- load order
local routines = {}
local listeners = {}   -- event name -> list of {fn=, owner=}
local depth = 0
local time = { dt = 0, now = 0, frame = 0 }
tbd.time = time

local function fail(inst, where, err)
  inst.dead = true
  tbd.log.info(("script error in %s (%s): %s -- instance disabled until the file is saved again"):format(inst.path, where, tostring(err)))
end

-- Base methods every script instance gets through its definition table.
local Base = {}
Base.__index = Base

function Base:run(fn)
  local co = coroutine.create(fn)
  table.insert(routines, { co = co, owner = self, wait = "none" })
  return co
end

function Base:on(name, fn)
  listeners[name] = listeners[name] or {}
  table.insert(listeners[name], { fn = fn, owner = self })
end

function Base:emit(name, ...) tbd.events.emit(name, ...) end

local function resume(r, ...)
  local ok, kind, a = coroutine.resume(r.co, ...)
  if not ok then
    r.done = true
    tbd.log.info(("script error in coroutine: %s"):format(tostring(kind)))
    if r.owner and r.owner.__inst then r.owner.__inst.dead = true end
    return
  end
  if coroutine.status(r.co) == "dead" then r.done = true return end
  r.wait = kind
  if kind == "time" then r.until_ = time.now + a
  elseif kind == "frames" then r.frames = a
  elseif kind == "pred" then r.pred = a
  elseif kind == "event" then r.event = a end
end

local function inScript() return coroutine.isyieldable() end

function tbd.wait(seconds)
  assert(inScript(), "tbd.wait can only be called inside self:run(function() ... end)")
  coroutine.yield("time", seconds or 0)
end
function tbd.wait_frames(n) assert(inScript(), "tbd.wait_frames needs self:run") coroutine.yield("frames", n or 1) end
function tbd.wait_until(pred) assert(inScript(), "tbd.wait_until needs self:run") coroutine.yield("pred", pred) end
function tbd.wait_event(name) assert(inScript(), "tbd.wait_event needs self:run") return coroutine.yield("event", name) end

tbd.events = {}
function tbd.events.on(name, fn)
  listeners[name] = listeners[name] or {}
  table.insert(listeners[name], { fn = fn })
end
function tbd.events.off(name, fn)
  local l = listeners[name]
  if not l then return end
  for i = #l, 1, -1 do if l[i].fn == fn then table.remove(l, i) end end
end
function tbd.events.emit(name, ...)
  if depth >= 16 then tbd.log.info("event '" .. name .. "' dropped: recursion depth 16") return end
  depth += 1
  local l = listeners[name]
  if l then
    for _, h in ipairs(table.clone(l)) do
      local inst = h.owner and h.owner.__inst
      if not (inst and inst.dead) then
        local ok, err = pcall(h.fn, h.owner, ...)
        if not ok then if inst then fail(inst, "event " .. name, err) else tbd.log.info("event handler error: " .. tostring(err)) end end
      end
    end
  end
  for _, r in ipairs(routines) do
    if not r.done and r.wait == "event" and r.event == name then r.wait = "none" resume(r, ...) end
  end
  depth -= 1
end

-- Entity handle given to entity scripts as self.entity. Entities are indices in the scene.
local EntityMT = {}
EntityMT.__index = EntityMT
function EntityMT:name() return tbd.scene.name(self.id) end
function EntityMT:position() return tbd.scene.position(self.id) end
function EntityMT:rotation() return tbd.scene.rotation(self.id) end
function EntityMT:scale() return tbd.scene.scale(self.id) end
function EntityMT:set_position(v) tbd.scene.set_position(self.id, v) end
function EntityMT:set_rotation(q) tbd.scene.set_rotation(self.id, q) end
function EntityMT:set_scale(v) tbd.scene.set_scale(self.id, v) end
function tbd.entity(id) return setmetatable({ id = id }, EntityMT) end

local defs = {}       -- script path -> definition table
local started = false

local function runOnLoad(inst, reloaded)
  local f = inst.def.on_load
  if f then local ok, err = pcall(f, inst.self, reloaded) if not ok then fail(inst, "on_load", err) end end
end

-- Called by the host with the table a script file returned. A path seen before is a hot reload: every live
-- instance keeps its self (the fields), gets the new code, and on_load(true) runs. Declared props that are new get
-- their defaults and removed ones are dropped.
function tbd.__define(path, def)
  if type(def) ~= "table" then tbd.log.info(path .. ": a script must return a table of hooks") return false end
  if getmetatable(def) == nil then setmetatable(def, Base) end
  local old = defs[path]
  defs[path] = def
  if old then
    local oldProps, newProps = old.props or {}, def.props or {}
    for _, inst in ipairs(instances) do
      if inst.path == path then
        inst.def = def
        getmetatable(inst.self).__index = def
        inst.dead = false
        for k in pairs(oldProps) do if newProps[k] == nil then inst.self[k] = nil end end
        for k, p in pairs(newProps) do if rawget(inst.self, k) == nil then inst.self[k] = p.default end end
        runOnLoad(inst, true)
      end
    end
  end
  return true
end

-- A new running copy of a defined script. entity is a scene index or nil for a global script; overrides maps prop
-- names to strings (from the scene file).
function tbd.__instance(path, entity, overrides)
  local def = defs[path]
  if not def then return false end
  local inst = { path = path, def = def, dead = false }
  local self_ = setmetatable({ __inst = inst }, { __index = def })
  inst.self = self_
  if entity then self_.entity = tbd.entity(entity) end
  for k, p in pairs(def.props or {}) do
    local v = p.default
    local s = overrides and overrides[k]
    if s ~= nil then
      if p.type == "number" then
        v = tonumber(s) or v
        if p.min and v < p.min then v = p.min end
        if p.max and v > p.max then v = p.max end
      elseif p.type == "bool" then v = (s == "true" or s == "1")
      else v = s end
    end
    self_[k] = v
  end
  table.insert(instances, inst)
  runOnLoad(inst, false)
  if started and def.on_start and not inst.dead then
    local ok, err = pcall(def.on_start, self_) if not ok then fail(inst, "on_start", err) end
  end
  return true
end

-- The declared props of a script, sorted by name, for the inspector.
function tbd.__props(path)
  local out = {}
  local def = defs[path]
  if not def or not def.props then return out end
  local names = {}
  for k in pairs(def.props) do table.insert(names, k) end
  table.sort(names)
  for _, k in ipairs(names) do
    local p = def.props[k]
    table.insert(out, { name = k, type = p.type or "string", default = tostring(p.default), min = p.min, max = p.max })
  end
  return out
end

function tbd.__hook(name, a)
  if name == "on_start" then started = true end
  for _, inst in ipairs(instances) do
    local f = inst.def[name]
    if f and not inst.dead then
      local ok, err = pcall(f, inst.self, a)
      if not ok then fail(inst, name, err) end
    end
  end
end

-- Resumes coroutines whose wait is over. Runs after on_update.
function tbd.__routines(dt)
  for i = #routines, 1, -1 do
    local r = routines[i]
    if r.done or (r.owner.__inst and r.owner.__inst.dead) then table.remove(routines, i)
    elseif r.wait == "none" then resume(r)
    elseif r.wait == "time" and time.now >= r.until_ then resume(r)
    elseif r.wait == "frames" then r.frames -= 1 if r.frames <= 0 then resume(r) end
    elseif r.wait == "pred" then
      local ok, v = pcall(r.pred)
      if not ok then r.done = true tbd.log.info("wait_until predicate error: " .. tostring(v))
      elseif v then resume(r) end
    end
  end
end

function tbd.__frame(dt)
  time.dt = dt
  time.now += dt
  time.frame += 1
end

-- Play stops: coroutines are cancelled, instances get on_stop and are dropped.
function tbd.__stop()
  tbd.__hook("on_stop")
  table.clear(routines)
  table.clear(listeners)
  table.clear(instances)
  started = false
  time.now = 0 time.frame = 0
end
)LUAU"
