-- BuildFilter (Lua): a loot filter for your build in The Slormancer, as a Lua mod for SlormAPI.
--
-- The same mod as BuildFilter.dll, written in Lua: put this file into <The Slormancer>\mods\lua\ (SlormAPI loads it
-- while the game runs). Install one of the two, not both: when BuildFilter.dll is loaded this script stands aside.
--
-- In the inventory the Filter-o-tron 3000 Mk.II button is cut to half width and "Build Filter" sits next to it; Build
-- Filter opens its own page in Friedrich's panel (tabs Rules / Gear / Extras / Pickup). Everything is drawn INTO the
-- game's own inventory bake and registered in obj_ui_next_gen.mba (the game's list of clickable UI elements) through
-- SlormAPI draw programs, so the game itself slides it with the menu, draws hover frames and plays the sounds. Our
-- elements call the "sink" script with "bf:<action>"; SlormAPI swallows those calls (a skip override) and logs them,
-- and this script reads the log. Filtered items are converted with the game's own Filter-o-tron conversion.
--
-- Settings: saved by SlormAPI (slorm.save_data) in mods\aurie\SlormAPI\data\lua_BuildFilter.json.
-- Loot log (Options > Mods > BuildFilter (Lua)): mods\aurie\SlormAPI\data\lua_BuildFilter.log.

local S = slorm
local null = S.null

S.mod{ name = "BuildFilter (Lua)", version = "1.0.1", author = "thothas",
       description = "Loot filter for your build: inventory > Build Filter" }

local SINK = "scr_blacksmith_open_filter_from_inventory"
local BTN_X, BTN_Y = 1537, 904       -- the Filter-o-tron button when its element is not listed
local VALUE_LEVEL = 100
local BOX = 0x030303
local SCAN_BUDGET = 0.002            -- seconds per frame; the rest of a burst waits for the next frames

local SLOTS = {
  { key = "helm", label = "Helmet" }, { key = "body", label = "Body Armor" }, { key = "shoulder", label = "Shoulders" },
  { key = "bracer", label = "Bracers" }, { key = "glove", label = "Gloves" }, { key = "boot", label = "Boots" },
  { key = "ring", label = "Rings" }, { key = "amulet", label = "Amulet" }, { key = "belt", label = "Belt" },
  { key = "cape", label = "Cape" } }
local TRAITS = { [0] = "Toughness", "Savagery", "Fury", "Determination", "Zeal", "Willpower", "Dexterity", "Bravery" }

-- ============================================================================
-- Small helpers
-- ============================================================================

local function num(v, d)
  if type(v) == "number" then return v end
  if type(v) == "boolean" then return v and 1 or 0 end
  if type(v) == "string" then return tonumber(v) or d or 0 end
  return d or 0
end

local function str(v)
  if type(v) == "string" then return v end
  if type(v) == "number" then return string.format("%g", v) end
  if type(v) == "boolean" then return v and "True" or "False" end
  return ""
end

local function truthy(v) return v == true or (type(v) == "number" and v ~= 0) end

local function find(list, value)
  for i, v in ipairs(list) do if v == value then return i end end
  return nil
end

local function range(first, last, step)
  local t = { 0 }
  for v = first, last, step or 1 do t[#t + 1] = v end
  return t
end
local GOLD_ROLLS, LEG_ROLLS, LEVELS = range(15, 40), range(75, 100), range(5, 100, 5)

local function step_in(values, cur, step)
  local i = find(values, cur) or 1
  i = math.max(1, math.min(#values, i + step))
  return values[i]
end

local function wrap(i, n) return (i - 1) % n + 1 end   -- 1-based index, wrapping

local function thousands(n)
  local s, out = tostring(math.floor(n)), ""
  for i = 1, #s do
    if i > 1 and (#s - i + 1) % 3 == 0 then out = out .. " " end
    out = out .. s:sub(i, i)
  end
  return out
end

local function same(a, b)          -- deep equality of plain data
  if type(a) ~= "table" or type(b) ~= "table" then return a == b end
  for k, v in pairs(a) do if not same(v, b[k]) then return false end end
  for k in pairs(b) do if a[k] == nil then return false end end
  return true
end

local function copy(t)
  if type(t) ~= "table" then return t end
  local c = {}
  for k, v in pairs(t) do c[k] = copy(v) end
  return c
end

-- ============================================================================
-- Game knowledge (from SlormAPI, cached)
-- ============================================================================

local cache = { slot = {}, leg = {}, skill = {}, value = {}, label = {} }
local cls = 0                          -- the current hero's class

local function slot_info(slot)
  local key = slot .. ":" .. cls
  if not cache.slot[key] then cache.slot[key] = S.slot_info(slot, cls) end
  return cache.slot[key]
end

local function stat_label(stat, slot)
  if stat == nil or stat == "" then return "(no stat)" end
  local key = stat .. ":" .. slot
  if not cache.label[key] then
    local ok, info = pcall(S.stat_info, stat, slot)
    cache.label[key] = ok and info.label or stat
  end
  return cache.label[key]
end

local function leg_name(id)
  if not cache.leg[id] then
    local ok, info = pcall(S.request, { cmd = "legendary_info", id = id })
    cache.leg[id] = ok and info.name or ("Legendary " .. id)
  end
  return cache.leg[id]
end

local function skill_name(hero, id)
  local key = hero .. ":" .. id
  if not cache.skill[key] then
    local ok, info = pcall(S.request, { cmd = "skill_info", hero = hero, id = id })
    cache.skill[key] = ok and info.name or ("Skill " .. id)
  end
  return cache.skill[key]
end

local function affix_value(stat, roll)
  local key = stat .. ":" .. roll
  if not cache.value[key] then cache.value[key] = S.stat_info(stat, nil, roll, VALUE_LEVEL).value end
  return cache.value[key]
end

-- ============================================================================
-- Settings (rules)
-- ============================================================================

local rules = {}
local rules_ver = 0                    -- bumped on every settings change (the UI state key)

local function slot_map(value)
  local m = {}
  for _, s in ipairs(SLOTS) do m[s.key] = copy(value) end
  return m
end

local EMPTY_ROWS = { { "", 0 }, { "", 0 }, { "", 0 } }    -- 3 gold stats per slot: { stat ("" = none), min roll }

local function ensure_defaults()
  local function def(k, v) if rules[k] == nil then rules[k] = v end end
  def("enabled", false)                  -- a fresh install starts off: set your stats first
  def("build", "your build")
  def("min_gold", 3)
  def("convert_to", "fragments")
  def("auto_collect", true)
  def("gold_pickup", true)
  def("tags", true)
  def("keep_all_legendaries", true)
  def("keep_pure", false)
  def("keep_extras", false)
  def("min_level", 0)
  def("extras_affinity", true)
  def("extras_attrs", {})
  def("extras_skills", {})
  def("reapers", {})
  def("gear_default", slot_map(EMPTY_ROWS))
  def("gear", copy(rules.gear_default))
  def("leg_default", slot_map(-1))       -- the legendary wanted per slot (-1 = none)
  def("leg_wanted", copy(rules.leg_default))
  def("leg_min_roll", slot_map(0))
  for _, s in ipairs(SLOTS) do           -- every slot present
    if type(rules.gear[s.key]) ~= "table" then rules.gear[s.key] = copy(EMPTY_ROWS) end
    if rules.leg_wanted[s.key] == nil then rules.leg_wanted[s.key] = -1 end
    if rules.leg_min_roll[s.key] == nil then rules.leg_min_roll[s.key] = 0 end
  end
end

local function load_rules()
  local ok, saved = pcall(S.load_data)
  rules = (ok and type(saved) == "table") and saved or {}
  ensure_defaults()
end

local function save_rules()
  rules_ver = rules_ver + 1
  S.save_data(rules)
end

-- ============================================================================
-- Items: parse, classify, convert
-- ============================================================================

local function parse_item(ar)
  if type(ar) ~= "table" or type(ar[1]) ~= "table" or #ar[1] < 4 then return nil end
  local h = ar[1]
  local it = { rarity = str(h[1]), slot = str(h[2]), level = math.floor(num(h[3])), kind = str(h[4]),
               reaper = str(h[5]), legendary = -1, leg_roll = 0, lines = {}, pure = false, extras = {}, raw = ar }
  for k = 2, #ar do
    local l = ar[k]
    if type(l) == "table" and #l > 0 then
      local tag = str(l[1])
      if tag == "L" and #l > 1 then
        it.legendary, it.leg_roll = math.floor(num(l[2])), num(l[3])
      elseif (tag == "RP" or tag == "MA" or tag == "AT") and #l > 2 then
        it.extras[#it.extras + 1] = { tag = tag, index = math.floor(num(l[2])), n = math.floor(num(l[3])) }
      elseif #l > 2 and type(l[2]) == "string" then
        it.lines[#it.lines + 1] = { tier = tag, stat = l[2], roll = num(l[3]) }
        if num(l[4]) > 0 then it.pure = true end
      end
    end
  end
  return it
end

local function gold_matches(gold, rows)
  local free = {}
  for _, r in ipairs(rows) do
    if type(r[1]) == "string" and r[1] ~= "" then free[#free + 1] = { r[1], num(r[2]) } end
  end
  local n = 0
  for _, g in ipairs(gold) do
    for i, f in ipairs(free) do
      if f[1] == g.stat and g.roll >= f[2] then table.remove(free, i); n = n + 1; break end
    end
  end
  return n
end

local function extras_match(it)
  if not rules.keep_extras then return "" end
  for _, e in ipairs(it.extras) do
    if e.tag == "MA" and find(rules.extras_skills, e.index) then
      return "+" .. e.n .. " " .. skill_name(cls, e.index)
    end
    if e.tag == "AT" and TRAITS[e.index] and find(rules.extras_attrs, e.index) then
      return "+" .. e.n .. " " .. TRAITS[e.index]
    end
    if e.tag == "RP" and rules.extras_affinity and rules.smith ~= nil and e.index == math.floor(num(rules.smith, -1)) then
      return "+" .. e.n .. " Affinity"
    end
  end
  return ""
end

-- verdict: BUILD / GOLD / PURE / EXTRA (kept, tagged), CONVERT, COLLECT, or "" (left alone); and the tag
local function classify(it)
  if it.kind ~= "equipment" then
    if it.kind == "weapon" or it.kind == "reaper" then
      local name = rules.reapers[it.reaper]
      if name ~= nil then return "BUILD", "BUILD " .. str(name) end
      return "", ""
    end
    return rules.auto_collect and "COLLECT" or "", ""
  end
  if it.rarity == "legendary" or it.rarity == "nether" or it.legendary >= 0 then
    if it.legendary >= 0 then
      for slot, id in pairs(rules.leg_wanted) do
        if num(id, -1) == it.legendary and it.leg_roll >= num(rules.leg_min_roll[slot]) then
          return "BUILD", "BUILD " .. leg_name(it.legendary)
        end
      end
      if it.rarity ~= "nether" and not rules.keep_all_legendaries then return "CONVERT", "" end
    end
    return "", ""
  end
  if it.level < num(rules.min_level) then return "CONVERT", "" end
  local gold = {}
  for _, l in ipairs(it.lines) do if l.tier == "E" then gold[#gold + 1] = l end end
  local rows = rules.gear[it.slot]
  local n = rows and gold_matches(gold, rows) or 0
  local need = math.min(math.floor(num(rules.min_gold, 3)), #gold)
  if #gold > 0 and n >= need and n >= 1 then return "GOLD", "GOLD " .. n .. "/" .. #gold end
  local extra = extras_match(it)
  if extra ~= "" then return "EXTRA", extra end
  if rules.keep_pure and it.pure then return "PURE", "PURE" end
  return "CONVERT", ""
end

local RARITY_ROW = { normal = 0, magic = 1, rare = 2, epic = 3, legendary = 4 }
local saved_filter                     -- the hero's own filter settings, read once per scan pass

-- The game's own Filter-o-tron conversion for this one item: the hero's filter row of its rarity set to "hide, no
-- exceptions" and its conversion to Fragments / Goldus for one scr_blacksmith_filter_mla call, then put back.
local function convert(hero, loot, it)
  local to = rules.convert_to
  if to ~= "fragments" and to ~= "goldus" then return false end
  local row = RARITY_ROW[it.rarity]
  if not row then return false end
  local path = "loot_filter[" .. cls .. "]"
  if saved_filter == nil then saved_filter = S.get(path, nil, 3) end
  -- one frame-consistent batch; the put-back runs even if the conversion call fails
  local r = S.request{ cmd = "batch", cmds = {
    { cmd = "set", path = path .. "[0]", value = 1 },
    { cmd = "set", path = path .. "[1][" .. row .. "]", value = { 1, 0, 0, 0, 0, 0, 0 } },
    { cmd = "set", path = path .. "[2]", value = to == "fragments" and 1 or 2 },
    { cmd = "call", fn = "scr_blacksmith_filter_mla", args = { it.raw }, self = S.inst(hero) },
    { cmd = "set", path = path, value = saved_filter } } }
  if r[4] and r[4].result == 0 then
    S.call("instance_destroy", S.inst(loot))
    return true
  end
  return false
end

-- ============================================================================
-- Scanning the ground
-- ============================================================================

local seen, seen_n = {}, 0
local counts = { BUILD = 0, GOLD = 0, PURE = 0, EXTRA = 0, CONVERT = 0, COLLECT = 0, COINS = 0 }
local KEEP = { BUILD = true, GOLD = true, PURE = true, EXTRA = true }
local loot_log = true                  -- setting "loot_log"

local function handle(hero, inst)
  local it = parse_item(S.get("ar", S.inst(inst), 3))
  if not it then return end
  local verdict, label = classify(it)
  if verdict == "COLLECT" then
    if truthy(S.call_on(S.inst(hero), "scr_pick_up_item", it.raw, true, false, true)) then
      S.call("instance_destroy", S.inst(inst))
    else
      seen[inst] = nil                   -- inventory full: try again later
      return
    end
  elseif verdict == "CONVERT" then
    if not convert(hero, inst, it) then S.set("visible", false, S.inst(inst)) end
  elseif KEEP[verdict] and rules.tags then
    local name = str(S.get("loot_name", S.inst(inst)))
    S.request{ cmd = "batch", cmds = {
      { cmd = "set", path = "loot_name", value = label .. "  " .. name, root = S.inst(inst) },
      { cmd = "set", path = "baked", value = false, root = S.inst(inst) } } }
  end
  if verdict ~= "" then
    counts[verdict] = counts[verdict] + 1
    if loot_log then
      local gold = {}
      for _, l in ipairs(it.lines) do
        if l.tier == "E" then gold[#gold + 1] = l.stat .. " " .. math.floor(l.roll) end
      end
      S.log_file(verdict .. " " .. (it.kind == "equipment"
        and (it.rarity .. " " .. it.slot .. " leg=" .. it.legendary .. " gold=[" .. table.concat(gold, ", ") .. "]"
             .. (label ~= "" and (" " .. label) or ""))
        or it.kind))
    end
  end
end

-- returns true when drops are left for the next frame
local function scan(hero)
  local backlog = false
  saved_filter = nil
  local loots = S.instances("obj_loot")
  local t0, handled = S.clock(), 0     -- the budget is for the drops: one always gets its turn
  for _, l in ipairs(loots) do
    local inst = l["$inst"]
    if not seen[inst] then
      if handled > 0 and S.clock() - t0 > SCAN_BUDGET then backlog = true; break end
      handled = handled + 1
      seen[inst] = true
      seen_n = seen_n + 1
      pcall(handle, hero, inst)          -- an error: picked up / despawned meanwhile
    end
  end
  if seen_n > 5000 then seen, seen_n = {}, 0 end
  if rules.gold_pickup and not backlog then
    -- gold coins: the coin's own pick-up flag, so it flies to the hero and the game adds it (gold find, sound)
    local cmds = {}
    for _, c in ipairs(S.instances("obj_gold_coin", { "pick_up_animation", "deny_loot", "value" })) do
      if not truthy(c.pick_up_animation) and not truthy(c.deny_loot) then
        cmds[#cmds + 1] = { cmd = "set", path = "pick_up_animation", value = true, root = S.inst(c["$inst"]) }
        counts.COINS = counts.COINS + num(c.value)
      end
    end
    if #cmds > 0 then S.request{ cmd = "batch", cmds = cmds } end
  end
  return backlog
end

-- ============================================================================
-- In-game UI: draw programs (see SlormAPI's README, "Draw programs")
-- ============================================================================

local sink = 0                         -- the sink script's index: our clickable elements call it with "bf:<action>"

local function op(name, ...) return { ["$op"] = name, args = { ... } } end
local function V(name) return { ["$v"] = name } end
local function asset(name) return { ["$asset"] = name } end
local function self_var(name) return { ["$self"] = name } end

local function text(x, y, s, font, ha, va, color, a8, a9, mark)
  local args = { x, y, s, 1, 1, font or "ft_standard_12", ha or 1, va or 1, a8 or 0, a9 or 1, color or "white", "black", 1,
                 mark ~= nil }
  if mark then args[15] = mark end
  return { fn = "scr_draw_text", args = args }
end
-- The game lays its menus out for 1920x1080 and multiplies positions and sizes by self.ui_scale (from the resolution)
-- as it draws; its mba entries and the panel's position arrive already scaled. "sc" is that scale (a program var).
local function px(n) return op("*", n, V("sc")) end
local SCALE_VAR = { "sc", op("??", self_var("ui_scale"), 1) }
local function spr(name, frame, x, y)
  return { fn = "draw_sprite_ext", args = { asset(name), frame, x, y, V("sc"), V("sc"), 0, 0xFFFFFF, 1 } }
end
local function push(entry, when) return { fn = "$mba_push", args = { entry }, when = when } end

local function inventory_program(on, ours)
  local shown = op("==", self_var("menu_citizen"), "blacksmith")
  local ours_shown = op("and", shown, ours)
  local x, y = V("bx"), V("by")
  local right = on and "spr_menu_red_button" or "spr_menu_grey_button"
  local c = {
    { fn = "$mba_set", args = { "Filter Active", 4, px(168) } },
    { fn = "$mba_set", args = { "Filter Active", 1, asset("spr_menu_grey_button") } },
    { fn = "$mba_set", args = { "Filter Active", 6, "Filter-o-tron" } },
    spr("spr_menu_grey_button", op("?", op("and", shown, not ours), 3, 0), x, y),
    text(op("+", x, px(84)), op("+", y, px(30)), "Filter-o-tron"),
    spr(right, op("?", ours_shown, 3, 0), op("+", x, px(168)), y),
    text(op("+", x, px(252)), op("+", y, px(30)), "Build Filter"),
    push({ "Build Filter", asset(right), op("+", x, px(168)), y, px(168), px(52), "Build Filter", sink, "bf:open" }, op("not", ours_shown)),
    push({ "Filter-o-tron", asset("spr_menu_grey_button"), x, y, px(168), px(52), "Filter-o-tron", sink, "bf:game" }, ours_shown),
  }
  return {
    vars = { SCALE_VAR, { "bx", op("??", op("mba", "Filter Active", 2), px(BTN_X)) },
             { "by", op("??", op("mba", "Filter Active", 3), px(BTN_Y)) } },
    when = op("or", op("def", op("mba", "Filter Active", 0)), op("and", shown, op("==", self_var("menu_show"), "inventory"))),
    calls = c }
end

-- Friedrich's panel: positions relative to the panel (dx, dy come from the hooked script's arguments)
local Panel = {}
Panel.__index = Panel
local function X(n) return op("+", px(n), V("dx")) end
local function Y(n) return op("+", px(n), V("dy")) end

function Panel.new() return setmetatable({ c = {} }, Panel) end
function Panel:add(call) self.c[#self.c + 1] = call end

function Panel:radio(yp, label, mark, on, action, big, xp, font)
  xp = xp or 184
  local sprite, extra = nil, ""
  if big then
    self:add(spr("spr_radio_btn_v3", 0, X(xp), Y(yp)))
    if on then self:add(spr("spr_radio_cross_v2", 0, X(xp + 18), Y(yp + 18))) end
    self:add(text(X(xp + 42), Y(yp + 10), label, "ft_standard_16", 0, 1, "white", 0, 1, mark))
    sprite = "spr_radio_btn_v3"
    if on then extra = 788 end           -- spr_radio_cross_v2: the game's "on" fill for hover
  else
    sprite = on and "spr_radio_small_btn_filled" or "spr_radio_small_btn"
    self:add(spr(sprite, 0, X(xp), Y(yp)))
    self:add(text(X(xp + 42), Y(yp + 18), label, font or "ft_standard_12", 0, 1, "white", 0, 1, mark))
  end
  if action then
    self:add(push({ "Filter", asset(sprite), X(xp), Y(yp), px(36), px(36), extra, sink, "bf:" .. action, "" }))
  end
end

function Panel:arrow(left, xp, yp, action)
  local sprite = left and "spr_selector_btn_l_v2" or "spr_selector_btn_r_v2"
  self:add(spr(sprite, 0, X(xp), Y(yp)))
  self:add(push({ "sel", asset(sprite), X(xp), Y(yp), px(24), px(36), "", sink, "bf:" .. action }))
end

function Panel:block(xp, yp, w, value, color, font)
  local square = w <= 72
  local name = square and "spr_selector_block_square" or "spr_selector_block_v2"
  local native = square and 36 or 256
  self:add({ fn = "draw_sprite_ext", args = { asset(name), 0, X(xp), Y(yp), px(w / native), V("sc"), 0, 0xFFFFFF, 1 } })
  self:add(text(X(xp + w / 2), Y(yp + 18), value, font or "ft_standard_12", 1, 1, color or "grey", 1, 0))
end

function Panel:selector(yp, value, l, r, xl, xr, wide)
  if wide then self:block(202, yp, 256, value) else self:block(312, yp, 36, value) end
  if l then self:arrow(true, xl or 284, yp, l) end
  if r then self:arrow(false, xr or 352, yp, r) end
end

function Panel:steps(values, cur, xl, xr, yp, minus, plus)
  local i = find(values, cur) or 1
  if i > 1 then self:arrow(true, xl, yp, minus) end
  if i < #values then self:arrow(false, xr, yp, plus) end
end

function Panel:button(xp, yp, label, action, selected, disabled)
  self:add(spr("spr_menu_grey_button", selected and 3 or 0, X(xp), Y(yp)))
  self:add(text(X(xp + 84), Y(yp + 30), label, "ft_standard_12", 1, 1, disabled and "grey" or "white"))
  if not (selected or disabled) then
    self:add(push({ "BF " .. label, asset("spr_menu_grey_button"), X(xp), Y(yp), px(168), px(52), label, sink, "bf:" .. action }))
  end
end

function Panel:tab(xp, yp, label, action, selected)
  local sprite = "spr_menu_floor_grey_button_done"     -- 88x52: 0 normal, 1 hover, 2 gold (selected)
  self:add(spr(sprite, selected and 2 or 0, X(xp), Y(yp)))
  self:add(text(X(xp + 44), Y(yp + 28), label, "ft_standard_11"))
  if not selected then
    self:add(push({ "BF " .. label, asset(sprite), X(xp), Y(yp), px(88), px(52), label, sink, "bf:" .. action }))
  end
end

function Panel:box(x0, y0, x1, y1)
  self:add({ fn = "draw_set_colour", args = { BOX } })
  self:add({ fn = "draw_rectangle", args = { X(x0), Y(y0), X(x1), Y(y1), false } })
  self:add({ fn = "draw_set_colour", args = { 0xFFFFFF } })
end

function Panel:wrapped(xp, yp, s, width, color)
  self:add(text(X(xp), Y(yp), { ["$fn"] = "scr_cut_text", args = { s, px(width), "ft_standard_12" } }, "ft_standard_12", 1, 1,
    color or "white"))
end

local function value_label(stat, min_roll)
  if stat == "" or min_roll <= 0 then return "any" end
  local v = affix_value(stat, min_roll)
  local pct = stat:sub(-8) == "_percent" or stat:sub(-5) == "_mult"
  local s = string.format("%g", v) .. (pct and "%" or "")
  return min_roll >= 40 and (s .. " max") or (">= " .. s)
end

local function build_skills()
  if type(rules.build_skills) == "table" and #rules.build_skills > 0 then return rules.build_skills end
  local v = {}                         -- the class's equipped skills
  local ok, eq = pcall(S.get, "skill_equipped[" .. cls .. "]", nil, 1)   -- equipped: 2..4
  if ok and type(eq) == "table" then
    for i = 1, math.min(#eq, 64) do
      local x = num(eq[i], -1)
      if x >= 2 and x <= 4 then v[#v + 1] = i - 1 end
    end
  end
  return v
end

local PAGES = { { "rules", "Rules" }, { "gear", "Gear" }, { "extras", "Extras" }, { "pickup", "Pickup" } }
local CONVERT_TO = { "fragments", "goldus", "hide" }
local CONVERT_LABEL = { fragments = "Fragments", goldus = "Goldus", hide = "Nothing (hide them)" }

local function panel_program(on, page, slot_i)
  local p = Panel.new()
  local g = math.floor(num(rules.min_gold, 3))
  local kept = counts.BUILD + counts.GOLD + counts.PURE + counts.EXTRA
  local done = "Kept " .. kept .. "   Converted " .. counts.CONVERT .. "   Picked up " .. counts.COLLECT
    .. "   Gold " .. thousands(counts.COINS)
  p:box(136, 348, 523, 467)
  local build = str(rules.build):gsub("%.json$", "")
  p:add(text(X(330), Y(370), "Loot Filter for your Build (" .. build .. ")", "ft_standard_12", 1, 1, "grey"))
  p:wrapped(330, 406, "Keeps your build's legendaries and gold stats; every other drop becomes materials.", 350)
  p:add(text(X(330), Y(446), done, "ft_standard_12", 1, 1, "grey"))
  for i, pg in ipairs(PAGES) do p:tab(136 + 100 * (i - 1), 478, pg[2], "page:" .. pg[1], page == pg[1]) end
  p:add(spr("spr_filter_slot_decoration", 0, X(330), Y(545)))

  if page == "rules" then
    p:radio(576, on and "BUILD FILTER {ON}" or "BUILD FILTER {OFF}", on and "pure" or "grey", on, "toggle", true)
    p:add(text(X(226), Y(618), "Keep a dropped item when...", "ft_standard_12", 0, 1, "white", 0, 1, "normal"))
    local all = rules.keep_all_legendaries
    p:radio(626, all and "... it is any {Legendary}" or "... it is a {Legendary} picked in Gear", "item_highlight", true, "legall")
    p:radio(662, "... it has {Gold Stats} the build wants:", "item_highlight", true)
    p:selector(698, tostring(g), g > 1 and "gold-" or nil, g < 3 and "gold+" or nil)
    p:add(text(X(390), Y(716), "of 3", "ft_standard_12", 0, 1, "grey"))
    p:radio(740, "... it has a {Pure Stat}", "pure", rules.keep_pure, "pure")
    p:radio(774, "... it has an {Extra} ticked in Extras", "item_highlight", rules.keep_extras, "extras")
    local lvl = math.floor(num(rules.min_level))
    p:box(136, 854, 523, 930)
    p:add(text(X(330), Y(878), "Convert equipment below item level:", "ft_standard_12", 1, 2, "grey"))
    p:block(202, 890, 256, lvl > 0 and ("level " .. lvl) or "any level")
    p:steps(LEVELS, lvl, 170, 466, 890, "lvl-", "lvl+")
  elseif page == "gear" then
    local slot = SLOTS[slot_i]
    local rows = rules.gear[slot.key]
    p:selector(560, slot.label, "slot-", "slot+", 170, 466, true)
    p:add(text(X(330), Y(610), "Gold stats to keep, minimum value (level " .. VALUE_LEVEL .. "):", "ft_standard_12", 1, 1, "grey"))
    for k = 1, 3 do
      local stat, min_roll = rows[k][1], math.floor(num(rows[k][2]))
      local y = 624 + 40 * (k - 1)
      local ks = tostring(k - 1)
      p:arrow(true, 142, y, "stat" .. ks .. "-")
      p:arrow(false, 358, y, "stat" .. ks .. "+")
      p:block(166, y, 190, stat_label(stat, slot.key), stat == "" and "grey" or "white", "ft_standard_11")
      p:block(426, y, 64, value_label(stat, min_roll), (stat ~= "" and min_roll > 0) and "white" or "grey", "ft_standard_11")
      if stat ~= "" then p:steps(GOLD_ROLLS, min_roll, 400, 492, y, "min" .. ks .. "-", "min" .. ks .. "+") end
    end
    local leg = math.floor(num(rules.leg_wanted[slot.key], -1))
    local lroll = math.floor(num(rules.leg_min_roll[slot.key]))
    p:add(text(X(330), Y(750), "Legendary to keep, minimum legendary roll:", "ft_standard_12", 1, 1, "grey"))
    p:arrow(true, 142, 764, "leg-")
    p:arrow(false, 388, 764, "leg+")
    p:block(166, 764, 220, leg >= 0 and leg_name(leg) or "(no legendary)", leg >= 0 and "item_highlight" or "grey", "ft_standard_11")
    p:block(442, 764, 48, lroll > 0 and (">= " .. lroll .. "%") or "any", (leg >= 0 and lroll > 0) and "white" or "grey", "ft_standard_11")
    if leg >= 0 then p:steps(LEG_ROLLS, lroll, 416, 492, 764, "lroll-", "lroll+") end
    local is_default = same(rules.gear_default[slot.key], rows) and num(rules.leg_default[slot.key], -1) == leg and lroll == 0
    p:box(136, 854, 523, 930)
    p:button(246, 866, "Reset slot", "reset", false, is_default)
  elseif page == "extras" then
    local on_x = rules.keep_extras
    p:radio(576, on_x and "KEEP {EXTRAS}" or "KEEP {EXTRAS} (off)", on_x and "item_highlight" or "grey", on_x, "extras", true)
    p:add(text(X(330), Y(622), "Skill levels (+Mastery) and reaper affinity:", "ft_standard_12", 1, 1, "grey"))
    local cells = {}
    for _, sid in ipairs(build_skills()) do
      cells[#cells + 1] = { skill_name(cls, sid), find(rules.extras_skills, sid) ~= nil, "xs:" .. sid }
    end
    cells[#cells + 1] = { "Reaper Affinity", rules.extras_affinity, "xrp" }
    for i, cell in ipairs(cells) do
      local j = i - 1
      p:radio(636 + 32 * (j // 2), cell[1], nil, cell[2], cell[3], false, 190 + 160 * (j % 2), "ft_standard_11")
    end
    local y0 = 636 + 32 * ((#cells + 1) // 2) + 12
    p:add(text(X(330), Y(y0), "Attribute points:", "ft_standard_12", 1, 1, "grey"))
    for i = 0, 7 do
      p:radio(y0 + 12 + 30 * (i // 2), TRAITS[i], nil, find(rules.extras_attrs, i) ~= nil, "xa:" .. i, false,
        190 + 160 * (i % 2), "ft_standard_11")
    end
    p:box(136, 854, 523, 930)
    p:wrapped(330, 892, "An item is kept when it has any ticked extra (and \"... it has an Extra\" is on in Rules).", 360, "grey")
  else
    p:radio(580, "Pick up {Materials} from anywhere", "pure", rules.auto_collect, "collect")
    p:radio(620, "Pick up {Gold} from anywhere", "item_highlight", rules.gold_pickup, "goldpick")
    p:radio(660, "Show {BUILD / GOLD} tags on kept drops", "item_highlight", rules.tags, "tags")
    p:wrapped(330, 740, "Materials and gold fly to you like normal pickups, so gold find and sounds are the game's own.", 340, "grey")
    p:box(136, 854, 523, 930)
    p:add(text(X(330), Y(878), "Turn filtered items into:", "ft_standard_12", 1, 2, "grey"))
    p:selector(890, CONVERT_LABEL[rules.convert_to] or "Fragments", "conv-", "conv+", 170, 466, true)
  end
  -- the panel's position arrives scaled (330, 156 at 1920x1080): our layout's offsets from it scale too
  return { vars = { SCALE_VAR, { "dx", op("-", { ["$arg"] = 0 }, px(330)) }, { "dy", op("-", { ["$arg"] = 1 }, px(156)) } },
           calls = p.c }
end

-- ============================================================================
-- Hooks (our own layer: SlormAPI gives this script the owner "lua:BuildFilter")
-- ============================================================================

local panel_hooked = false

local function guard()
  S.request{ cmd = "hook", script = SINK, capture = true, depth = 1, capture_args = { "bf:*" },
             overrides = { { args = { "bf:*" }, skip = true } } }
end

local function apply_ui(on, ours, page, slot_i)
  guard()
  S.request{ cmd = "hook", script = "scr_ui_menus_inventory", capture = false, after = inventory_program(on, ours) }
  local texts = { { args = { null, null, "Filter-o-tron 3000 Mk.II", null, null, "ft_standard_12" }, skip = true } }
  if ours then
    texts[2] = { args = { null, null, "Filter-o-tron 3000 Mk.II", null, null, "ft_standard_14" },
                 set_args = { ["2"] = "Build Filter" } }
  end
  S.request{ cmd = "hook", script = "scr_draw_text", capture = false, overrides = texts }
  if ours then
    S.request{ cmd = "hook", script = "scr_blacksmith_panel_filter", capture = false,
               overrides = { { args = {}, skip = true } }, after = panel_program(on, page, slot_i) }
    panel_hooked = true
  elseif panel_hooked then
    S.request{ cmd = "unhook", script = "scr_blacksmith_panel_filter" }
    panel_hooked = false
  end
end

local function our_hooks_present(ours)
  local have = {}
  for _, h in ipairs(S.request{ cmd = "hooks" }) do
    for _, l in ipairs(h.layers or {}) do
      if l.owner == S.owner then have[h.script] = true end
    end
  end
  for _, s in ipairs({ SINK, "scr_ui_menus_inventory", "scr_draw_text" }) do
    if not have["gml_Script_" .. s] then return false end
  end
  return not ours or have["gml_Script_scr_blacksmith_panel_filter"] == true
end

-- ============================================================================
-- Actions (a click on one of our elements)
-- ============================================================================

local st = { ready = false, failed = false, ui = 0, ours = false, page = "rules", slot_i = 1, applied = "",
             next_poll = 0, check_hooks_at = 0, next_scan = 0, retry_at = 0, inventory_open = false, show = "", citizen = "" }

local function gear_click(slot, act)
  local rows = rules.gear[slot]
  local k = tonumber(act:sub(-2, -2)) + 1
  if not rows[k] then return end
  local step = act:sub(-1) == "+" and 1 or -1
  if act:sub(1, 4) == "stat" then
    -- the build's stats for this slot first, then the build's other stats, then the rest by name
    local here, build, names = {}, {}, {}
    for _, r in ipairs(rules.gear_default[slot] or {}) do if r[1] ~= "" then here[r[1]] = true end end
    for _, s in ipairs(rules.build_stats or {}) do build[s] = true end
    local stats = {}
    for _, e in ipairs(slot_info(slot).epic_stats) do stats[#stats + 1] = e.stat; names[e.stat] = e.name end
    local function rank(s) return here[s] and 0 or build[s] and 1 or 2 end
    table.sort(stats, function(a, b)
      if rank(a) ~= rank(b) then return rank(a) < rank(b) end
      return names[a] < names[b]
    end)
    local pool = { "" }
    for _, s in ipairs(stats) do pool[#pool + 1] = s end
    local taken = {}
    for i, r in ipairs(rows) do if i ~= k and r[1] ~= "" then taken[r[1]] = true end end
    local i = find(pool, rows[k][1]) or 1
    for _ = 1, #pool do
      i = wrap(i + step, #pool)
      if not taken[pool[i]] then break end
    end
    rows[k] = { pool[i], pool[i] == "" and 0 or num(rows[k][2]) }
  else
    rows[k] = { rows[k][1], step_in(GOLD_ROLLS, math.floor(num(rows[k][2])), step) }
  end
end

local function toggle_in_list(key, value)
  local list = rules[key]
  local i = find(list, value)
  if i then table.remove(list, i) else list[#list + 1] = value end
  table.sort(list)
end

local function cycle(list, cur, step) return list[wrap((find(list, cur) or 1) + step, #list)] end

local function handle_action(act)
  local slot = SLOTS[st.slot_i].key
  if act == "open" then
    st.ours = true
    apply_ui(rules.enabled, true, st.page, st.slot_i)      -- our page first, then the panel opens
    if str(S.get("menu_citizen", S.inst(st.ui))) ~= "blacksmith" then S.call_on(S.inst(st.ui), SINK) end
  elseif act == "game" then st.ours = false
  elseif act == "toggle" then rules.enabled = not rules.enabled
  elseif act == "gold-" or act == "gold+" then
    rules.min_gold = math.max(1, math.min(3, math.floor(num(rules.min_gold, 3)) + (act == "gold+" and 1 or -1)))
  elseif act == "collect" then rules.auto_collect = not rules.auto_collect
  elseif act == "goldpick" then rules.gold_pickup = not rules.gold_pickup
  elseif act == "tags" then rules.tags = not rules.tags
  elseif act == "pure" then rules.keep_pure = not rules.keep_pure
  elseif act == "extras" then rules.keep_extras = not rules.keep_extras
  elseif act == "legall" then rules.keep_all_legendaries = not rules.keep_all_legendaries
  elseif act == "xrp" then rules.extras_affinity = not rules.extras_affinity
  elseif act:sub(1, 3) == "xs:" then toggle_in_list("extras_skills", tonumber(act:sub(4)))
  elseif act:sub(1, 3) == "xa:" then toggle_in_list("extras_attrs", tonumber(act:sub(4)))
  elseif act == "conv-" or act == "conv+" then rules.convert_to = cycle(CONVERT_TO, rules.convert_to, act == "conv+" and 1 or -1)
  elseif act:sub(1, 5) == "page:" then st.page = act:sub(6)
  elseif act == "slot-" or act == "slot+" then st.slot_i = wrap(st.slot_i + (act == "slot+" and 1 or -1), #SLOTS)
  elseif act:sub(1, 4) == "stat" or act:sub(1, 3) == "min" then gear_click(slot, act)
  elseif act == "leg-" or act == "leg+" then
    local pool = { -1 }
    for _, l in ipairs(slot_info(slot).legendaries) do pool[#pool + 1] = l.id end
    rules.leg_wanted[slot] = cycle(pool, math.floor(num(rules.leg_wanted[slot], -1)), act == "leg+" and 1 or -1)
  elseif act == "lroll-" or act == "lroll+" then
    rules.leg_min_roll[slot] = step_in(LEG_ROLLS, math.floor(num(rules.leg_min_roll[slot])), act == "lroll+" and 1 or -1)
  elseif act == "lvl-" or act == "lvl+" then
    rules.min_level = step_in(LEVELS, math.floor(num(rules.min_level)), act == "lvl+" and 1 or -1)
  elseif act == "reset" then
    rules.gear[slot] = copy(rules.gear_default[slot] or EMPTY_ROWS)
    rules.leg_wanted[slot] = rules.leg_default[slot] or -1
    rules.leg_min_roll[slot] = 0
  end
  save_rules()
end

-- ============================================================================
-- Settings in Options > Mods: the on/off hotkey and the loot log
-- ============================================================================

local hotkey, hotkey_down = 0, false

local function apply_setting(key, value)
  if key == "hotkey" then
    local n = tonumber(tostring(value):match("^F(%d+)$"))
    hotkey = (n and n >= 1 and n <= 12) and (111 + n) or 0   -- F1 = 112 (the game's vk_f1)
    hotkey_down = true                                      -- a press from before does not count
  elseif key == "loot_log" then
    loot_log = value == true
  end
end

for k, v in pairs(S.settings{
  { key = "hotkey", label = "Hotkey: Build Filter on / off", help = "Works while the game window has the focus",
    type = "choice", options = { "Off", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" }, default = "Off" },
  { key = "loot_log", label = "Loot log", help = "Write every kept / converted drop to lua_BuildFilter.log",
    type = "bool", default = true } }) do
  apply_setting(k, v)
end
S.on("setting_changed", function(e) if e.mod == S.display_name() then apply_setting(e.key, e.value) end end)
S.on("loot_dropped", function() st.next_scan = 0 end)   -- a drop is checked the moment it lands

-- ============================================================================
-- Main loop (every frame)
-- ============================================================================

local function init()
  load_rules()
  local ui = S.instances("obj_ui_next_gen")
  if #ui == 0 then error("no UI yet") end
  st.ui = ui[1]["$inst"]
  sink = S.call("asset_get_index", SINK)
  if S.call("script_get_name", sink) ~= SINK then error("click sink script not found") end
  S.request{ cmd = "hooklog", max = 1000000 }                 -- drop stale entries
  st.ready, st.applied = true, ""
  S.log("ready")
end

local function step(now)
  local pressed = false
  if hotkey ~= 0 then
    local down = truthy(S.call("keyboard_check", hotkey))     -- the game sees keys only while it has the focus
    pressed = down and not hotkey_down
    hotkey_down = down
  end
  -- poll every frame while the inventory is open (clicks), else 10 times a second: one batched request
  if st.inventory_open or now >= st.next_poll or pressed then
    st.next_poll = now + 0.1
    local r = S.request{ cmd = "batch", cmds = {
      { cmd = "get", path = "current_hero" },
      { cmd = "get", path = "menu_show", root = S.inst(st.ui) },
      { cmd = "get", path = "menu_citizen", root = S.inst(st.ui) },
      { cmd = "hooklog", max = 1000 } } }
    for _, sub in ipairs(r) do if not sub.ok then error(sub.error or "SlormAPI error") end end
    cls = math.floor(num(r[1].result))
    st.show, st.citizen = str(r[2].result), str(r[3].result)
    st.inventory_open = st.show == "inventory"
    local acted = false
    for _, e in ipairs(r[4].result.entries or {}) do
      local a = e.args and e.args[1]
      if type(a) == "string" and a:sub(1, 3) == "bf:" then
        handle_action(a:sub(4))
        acted = true
      end
    end
    if pressed then
      rules.enabled = not rules.enabled
      save_rules()
    end
    -- (the menu state was read before this frame's clicks: an "open" is re-checked on the next frame)
    if not acted and st.ours and (st.show ~= "inventory" or st.citizen ~= "blacksmith") then st.ours = false end
    local cs = ""
    if st.ours then
      for _, k in ipairs({ "BUILD", "COINS", "COLLECT", "CONVERT", "EXTRA", "GOLD", "PURE" }) do cs = cs .. counts[k] .. "," end
    end
    local state = (rules.enabled and "1" or "0") .. (st.ours and "1" or "0") .. st.page .. "," .. st.slot_i .. ","
      .. rules_ver .. "," .. cs
    local missing = false
    if now >= st.check_hooks_at then
      st.check_hooks_at = now + 2
      missing = not our_hooks_present(st.ours)
    end
    if state ~= st.applied or missing then
      apply_ui(rules.enabled, st.ours, st.page, st.slot_i)
      if st.inventory_open then S.set("rebake_menu", true, S.inst(st.ui)) end
      st.applied = state
    end
  end
  if rules.enabled and now >= st.next_scan then
    st.next_scan = now + 0.25
    local heroes = S.instances("obj_parent_hero")
    if #heroes > 0 and scan(heroes[1]["$inst"]) then st.next_scan = now end   -- backlog: next frame
  end
end

-- BuildFilter.dll does the same: with both installed, this script stands aside (the Mods page shows why)
local function dll_loaded()
  for _, m in ipairs(S.request{ cmd = "mods" }) do
    if type(m.file) == "string" and m.file:lower() == "buildfilter.dll" and m.loaded then return true end
  end
  return false
end

local tick_id, conflict_at = nil, 0

local function tick()
  local now = S.clock()
  if now >= conflict_at then
    conflict_at = now + 5
    if dll_loaded() then
      S.cancel(tick_id)
      S.request{ cmd = "unhook" }
      error("BuildFilter.dll is installed too, so this Lua version stays off: keep one of the two")
    end
  end
  if not st.ready then
    if now < st.retry_at then return end
    local ok, err = pcall(init)
    if not ok then
      if not st.failed then S.log("waiting: " .. tostring(err)) end
      st.failed, st.retry_at = true, now + 3
      return
    end
  end
  local ok, err = pcall(step, now)
  if not ok then
    S.log(tostring(err))
    st.ready, st.retry_at = false, now + 2                   -- re-init (e.g. the UI object was recreated)
  end
end

if dll_loaded() then error("BuildFilter.dll is installed too, so this Lua version stays off: keep one of the two") end
tick_id = S.every(0, tick)
