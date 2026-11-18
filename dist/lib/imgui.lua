-- MoonImGui (PC MoonLoader `require 'imgui'`) API on top of mimgui - MonetLoader for MP 4.0.0
--
-- PC scripts written for MoonImGui 1.1.5 (imgui.Process / imgui.OnDrawFrame / imgui.ImBool / imgui.ImBuffer ...)
-- run unchanged: every Im* value wraps a real mimgui (cimgui) buffer, widgets that take values are translated to
-- the mimgui pointer calls, and everything else falls through to mimgui itself (same Dear ImGui 1.7x API), so
-- imgui.Begin, imgui.Checkbox, imgui.InputText, imgui.Combo, imgui.SliderFloat, imgui.ColorEdit4 ... work the
-- same way as on PC. imgui.Process drives a mimgui.OnFrame; imgui.ShowCursor / imgui.LockPlayer map to the frame's
-- HideCursor / LockPlayer.
--
-- Licensed under the MIT License (same as mimgui / MoonImGui by FYP).

local ffi = require 'ffi'
local mimgui = require 'mimgui'
local new = mimgui.new

local imgui = {}
imgui._VERSION = '1.1.5-monet'
imgui._MIMGUI = mimgui
imgui.Process = false
imgui.ShowCursor = true
imgui.LockPlayer = false
imgui.LockMovement = false

-- ---------------------------------------------------------------------------------------------------------------
-- Im* value types (field .v like MoonImGui)
-- ---------------------------------------------------------------------------------------------------------------
local ImValue = {}
local function is_value(x) return type(x) == 'table' and rawget(x, '__imv') ~= nil end

local function make_scalar(ctype, default)
    return function(v)
        local o = {__imv = true, __ptr = new[ctype](v == nil and default or v)}
        return setmetatable(o, ImValue)
    end
end

local function make_array(ctype, n)
    return function(...)
        local args = {...}
        if type(args[1]) == 'table' and not is_value(args[1]) then args = args[1] end
        local init = {}
        for i = 1, n do init[i] = args[i] or 0 end
        local o = {__imv = true, __ptr = new[ctype][n](init), __n = n}
        return setmetatable(o, ImValue)
    end
end

ImValue.__index = function(t, k)
    local p = rawget(t, '__ptr')
    if k == 'v' then
        local n = rawget(t, '__n')
        if rawget(t, '__buf') then return ffi.string(p) end
        if n then
            local r = {}
            for i = 1, n do r[i] = p[i - 1] end
            return r
        end
        return p[0]
    end
    return nil
end
ImValue.__newindex = function(t, k, val)
    local p = rawget(t, '__ptr')
    if k == 'v' then
        local n = rawget(t, '__n')
        if rawget(t, '__buf') then
            mimgui.StrCopy(p, tostring(val), rawget(t, '__size'))
        elseif n then
            for i = 1, n do p[i - 1] = val[i] or 0 end
        else
            p[0] = val
        end
        return
    end
    rawset(t, k, val)
end
ImValue.__tostring = function(t) return tostring(t.v) end

imgui.ImBool = make_scalar('bool', false)
imgui.ImInt = make_scalar('int', 0)
imgui.ImFloat = make_scalar('float', 0)
imgui.ImInt2 = make_array('int', 2)
imgui.ImInt3 = make_array('int', 3)
imgui.ImInt4 = make_array('int', 4)
imgui.ImFloat2 = make_array('float', 2)
imgui.ImFloat3 = make_array('float', 3)
imgui.ImFloat4 = make_array('float', 4)
function imgui.ImBuffer(a, b)
    local size, text = 256, ''
    if type(a) == 'number' then size = a; text = b or '' elseif type(a) == 'string' then text = a; size = b or math.max(256, #a + 1) end
    local o = {__imv = true, __buf = true, __size = size, __ptr = new.char[size]()}
    setmetatable(o, ImValue)
    o.v = text
    return o
end

imgui.ImVec2 = mimgui.ImVec2
imgui.ImVec4 = mimgui.ImVec4
imgui.ImColor = mimgui.ImColor

local function P(x) if is_value(x) then return rawget(x, '__ptr') end return x end

-- ---------------------------------------------------------------------------------------------------------------
-- widgets whose signature differs from mimgui
-- ---------------------------------------------------------------------------------------------------------------
local function cstr_array(items)
    local arr = new['const char*'][#items](items)
    return arr, #items
end

function imgui.InputText(label, buf, flags, callback, user_data)
    return mimgui.InputText(label, P(buf), rawget(buf, '__size'), flags or 0, callback, user_data)
end
function imgui.InputTextMultiline(label, buf, size, flags, callback, user_data)
    return mimgui.InputTextMultiline(label, P(buf), rawget(buf, '__size'), size or imgui.ImVec2(0, 0), flags or 0, callback, user_data)
end
if mimgui.InputTextWithHint then
    function imgui.InputTextWithHint(label, hint, buf, flags, callback, user_data)
        return mimgui.InputTextWithHint(label, hint, P(buf), rawget(buf, '__size'), flags or 0, callback, user_data)
    end
end
function imgui.Combo(label, current, items, height)
    if type(items) == 'string' then
        return mimgui.ComboStr(label, P(current), items, height or -1)
    end
    local arr, n = cstr_array(items)
    return mimgui.Combo(label, P(current), arr, n, height or -1)
end
function imgui.ListBox(label, current, items, height)
    local arr, n = cstr_array(items)
    return mimgui.ListBoxStr_arr(label, P(current), arr, n, height or -1)
end
function imgui.RadioButton(label, a, b)
    if is_value(a) then return mimgui.RadioButtonIntPtr(label, P(a), b) end
    return mimgui.RadioButtonBool(label, a and true or false)
end
function imgui.Selectable(label, selected, flags, size)
    if is_value(selected) then return mimgui.SelectableBoolPtr(label, P(selected), flags or 0, size or imgui.ImVec2(0, 0)) end
    return mimgui.Selectable(label, selected and true or false, flags or 0, size or imgui.ImVec2(0, 0))
end
function imgui.MenuItem(label, shortcut, selected, enabled)
    if is_value(selected) then return mimgui.MenuItemBoolPtr(label, shortcut, P(selected), enabled == nil and true or enabled) end
    return mimgui.MenuItemBool(label, shortcut, selected and true or false, enabled == nil and true or enabled)
end
function imgui.CollapsingHeader(label, a, b)
    if is_value(a) then return mimgui.CollapsingHeaderBoolPtr(label, P(a), b or 0) end
    return mimgui.CollapsingHeader(label, a or 0)
end
function imgui.Text(fmt, ...)
    if select('#', ...) > 0 then fmt = string.format(fmt, ...) end
    return mimgui.Text('%s', tostring(fmt))
end
function imgui.TextColored(col, fmt, ...)
    if select('#', ...) > 0 then fmt = string.format(fmt, ...) end
    return mimgui.TextColored(col, '%s', tostring(fmt))
end
function imgui.TextDisabled(fmt, ...)
    if select('#', ...) > 0 then fmt = string.format(fmt, ...) end
    return mimgui.TextDisabled('%s', tostring(fmt))
end
function imgui.TextWrapped(fmt, ...)
    if select('#', ...) > 0 then fmt = string.format(fmt, ...) end
    return mimgui.TextWrapped('%s', tostring(fmt))
end
function imgui.BulletText(fmt, ...)
    if select('#', ...) > 0 then fmt = string.format(fmt, ...) end
    return mimgui.BulletText('%s', tostring(fmt))
end
function imgui.SetTooltip(fmt, ...)
    if select('#', ...) > 0 then fmt = string.format(fmt, ...) end
    return mimgui.SetTooltip('%s', tostring(fmt))
end
function imgui.LabelText(label, fmt, ...)
    if select('#', ...) > 0 then fmt = string.format(fmt, ...) end
    return mimgui.LabelText(label, '%s', tostring(fmt))
end
function imgui.Begin(name, open, flags)
    return mimgui.Begin(name, P(open), flags or 0)
end
function imgui.BeginPopupModal(name, open, flags)
    return mimgui.BeginPopupModal(name, P(open), flags or 0)
end
function imgui.SwitchContext() end
imgui.GetMiddleButtonWidth = nil

-- ---------------------------------------------------------------------------------------------------------------
-- frame driver: imgui.Process / imgui.OnDrawFrame / imgui.BeforeDrawFrame
-- ---------------------------------------------------------------------------------------------------------------
mimgui.OnInitialize(function()
    local f = rawget(imgui, 'BeforeDrawFrame')
    if type(f) == 'function' then pcall(f) end
end)
mimgui.OnFrame(function() return rawget(imgui, 'Process') == true and type(rawget(imgui, 'OnDrawFrame')) == 'function' end,
    function(frame)
        frame.HideCursor = not rawget(imgui, 'ShowCursor')
        frame.LockPlayer = rawget(imgui, 'LockPlayer') or rawget(imgui, 'LockMovement') or false
        local ok, err = pcall(imgui.OnDrawFrame)
        if not ok then
            print('[imgui] OnDrawFrame: ' .. tostring(err))
            imgui.Process = false
        end
    end)

-- everything else is mimgui (identical Dear ImGui API); value arguments are unwrapped on the way
local wrapped = {}
setmetatable(imgui, {__index = function(t, k)
    local v = mimgui[k]
    if type(v) == 'function' then
        local w = wrapped[k]
        if not w then
            w = function(...)
                local n = select('#', ...)
                if n == 0 then return v() end
                local a = {...}
                for i = 1, n do if is_value(a[i]) then a[i] = rawget(a[i], '__ptr') end end
                return v(unpack(a, 1, n))
            end
            wrapped[k] = w
        end
        return w
    end
    return v
end})

return imgui
