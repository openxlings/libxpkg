-- xim.libxpkg.json: lightweight pure-Lua JSON encoder/decoder
local M = {}

-- ---- Decoder ----

local function skip_whitespace(s, pos)
    return s:match("^%s*()", pos)
end

local function decode_error(s, pos, msg)
    local line = 1
    for _ in s:sub(1, pos):gmatch("\n") do line = line + 1 end
    error(string.format("json decode error at line %d, pos %d: %s", line, pos, msg))
end

local escape_map = {
    ['"'] = '"', ['\\'] = '\\', ['/'] = '/',
    ['b'] = '\b', ['f'] = '\f', ['n'] = '\n', ['r'] = '\r', ['t'] = '\t',
}

local function decode_string(s, pos)
    pos = pos + 1  -- skip opening quote
    local buf = {}
    while pos <= #s do
        local c = s:sub(pos, pos)
        if c == '"' then
            return table.concat(buf), pos + 1
        elseif c == '\\' then
            pos = pos + 1
            local esc = s:sub(pos, pos)
            if escape_map[esc] then
                table.insert(buf, escape_map[esc])
                pos = pos + 1
            elseif esc == 'u' then
                local hex = s:sub(pos + 1, pos + 4)
                local cp = tonumber(hex, 16)
                if not cp then decode_error(s, pos, "invalid unicode escape") end
                if cp < 0x80 then
                    table.insert(buf, string.char(cp))
                elseif cp < 0x800 then
                    table.insert(buf, string.char(
                        0xC0 + math.floor(cp / 64),
                        0x80 + (cp % 64)
                    ))
                else
                    table.insert(buf, string.char(
                        0xE0 + math.floor(cp / 4096),
                        0x80 + math.floor((cp % 4096) / 64),
                        0x80 + (cp % 64)
                    ))
                end
                pos = pos + 5
            else
                decode_error(s, pos, "invalid escape: \\" .. esc)
            end
        else
            table.insert(buf, c)
            pos = pos + 1
        end
    end
    decode_error(s, pos, "unterminated string")
end

local function decode_number(s, pos)
    local num_str = s:match("^-?%d+%.?%d*[eE]?[+-]?%d*", pos)
    if not num_str then decode_error(s, pos, "invalid number") end
    local val = tonumber(num_str)
    if not val then decode_error(s, pos, "invalid number: " .. num_str) end
    return val, pos + #num_str
end

local decode_value  -- forward declaration

local function decode_array(s, pos)
    pos = pos + 1  -- skip '['
    local arr = {}
    pos = skip_whitespace(s, pos)
    if s:sub(pos, pos) == ']' then return arr, pos + 1 end
    while true do
        local val
        val, pos = decode_value(s, pos)
        table.insert(arr, val)
        pos = skip_whitespace(s, pos)
        local c = s:sub(pos, pos)
        if c == ']' then return arr, pos + 1 end
        if c ~= ',' then decode_error(s, pos, "expected ',' or ']'") end
        pos = skip_whitespace(s, pos + 1)
    end
end

local function decode_object(s, pos)
    pos = pos + 1  -- skip '{'
    local obj = {}
    pos = skip_whitespace(s, pos)
    if s:sub(pos, pos) == '}' then return obj, pos + 1 end
    while true do
        pos = skip_whitespace(s, pos)
        if s:sub(pos, pos) ~= '"' then decode_error(s, pos, "expected string key") end
        local key
        key, pos = decode_string(s, pos)
        pos = skip_whitespace(s, pos)
        if s:sub(pos, pos) ~= ':' then decode_error(s, pos, "expected ':'") end
        pos = skip_whitespace(s, pos + 1)
        local val
        val, pos = decode_value(s, pos)
        obj[key] = val
        pos = skip_whitespace(s, pos)
        local c = s:sub(pos, pos)
        if c == '}' then return obj, pos + 1 end
        if c ~= ',' then decode_error(s, pos, "expected ',' or '}'") end
        pos = pos + 1
    end
end

decode_value = function(s, pos)
    pos = skip_whitespace(s, pos)
    local c = s:sub(pos, pos)
    if c == '"' then return decode_string(s, pos) end
    if c == '{' then return decode_object(s, pos) end
    if c == '[' then return decode_array(s, pos) end
    if c == 't' then
        if s:sub(pos, pos + 3) == "true" then return true, pos + 4 end
        decode_error(s, pos, "invalid value")
    end
    if c == 'f' then
        if s:sub(pos, pos + 4) == "false" then return false, pos + 5 end
        decode_error(s, pos, "invalid value")
    end
    if c == 'n' then
        if s:sub(pos, pos + 3) == "null" then return nil, pos + 4 end
        decode_error(s, pos, "invalid value")
    end
    if c == '-' or (c >= '0' and c <= '9') then return decode_number(s, pos) end
    decode_error(s, pos, "unexpected character: " .. c)
end

function M.decode(str)
    if type(str) ~= "string" then error("json.decode: expected string, got " .. type(str)) end
    local val, pos = decode_value(str, 1)
    return val
end

-- ---- Encoder ----

local function is_array(t)
    local max_i = 0
    local count = 0
    for k, _ in pairs(t) do
        if type(k) ~= "number" or k < 1 or math.floor(k) ~= k then return false end
        if k > max_i then max_i = k end
        count = count + 1
    end
    return max_i == count
end

local encode_value  -- forward declaration

local escape_char_map = {
    ['\\'] = '\\\\', ['"'] = '\\"', ['\b'] = '\\b',
    ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t',
}

local function encode_string(val)
    return '"' .. val:gsub('[\\"\b\f\n\r\t]', escape_char_map):gsub(
        "[\x00-\x1f]", function(c) return string.format("\\u%04x", c:byte()) end
    ) .. '"'
end

local function encode_array(arr, indent, level)
    if #arr == 0 then return "[]" end
    local items = {}
    for _, v in ipairs(arr) do
        table.insert(items, encode_value(v, indent, level))
    end
    if indent then
        local pad = string.rep(indent, level)
        local inner_pad = string.rep(indent, level + 1)
        return "[\n" .. inner_pad .. table.concat(items, ",\n" .. inner_pad) .. "\n" .. pad .. "]"
    end
    return "[" .. table.concat(items, ",") .. "]"
end

local function encode_object(obj, indent, level)
    local keys = {}
    for k in pairs(obj) do table.insert(keys, k) end
    if #keys == 0 then return "{}" end
    table.sort(keys)
    local items = {}
    for _, k in ipairs(keys) do
        local key_str = encode_string(tostring(k))
        local val_str = encode_value(obj[k], indent, level)
        if indent then
            table.insert(items, key_str .. ": " .. val_str)
        else
            table.insert(items, key_str .. ":" .. val_str)
        end
    end
    if indent then
        local pad = string.rep(indent, level)
        local inner_pad = string.rep(indent, level + 1)
        return "{\n" .. inner_pad .. table.concat(items, ",\n" .. inner_pad) .. "\n" .. pad .. "}"
    end
    return "{" .. table.concat(items, ",") .. "}"
end

encode_value = function(val, indent, level)
    level = level or 0
    local vtype = type(val)
    if val == nil then return "null" end
    if vtype == "boolean" then return val and "true" or "false" end
    if vtype == "number" then
        if val ~= val then return "null" end  -- NaN
        if val == math.huge or val == -math.huge then return "null" end
        if val == math.floor(val) and math.abs(val) < 1e15 then
            return string.format("%d", val)
        end
        return tostring(val)
    end
    if vtype == "string" then return encode_string(val) end
    if vtype == "table" then
        if is_array(val) then
            return encode_array(val, indent, level)
        else
            return encode_object(val, indent, level)
        end
    end
    error("json.encode: unsupported type: " .. vtype)
end

function M.encode(val, opts)
    opts = opts or {}
    local indent = opts.indent and (type(opts.indent) == "string" and opts.indent or "  ") or nil
    return encode_value(val, indent, 0)
end

-- File convenience functions: json.loadfile / json.savefile
function M.loadfile(filepath)
    local f = io.open(filepath, "r")
    if not f then return nil end
    local content = f:read("*a")
    f:close()
    if not content or content == "" then return nil end
    local ok, val = pcall(M.decode, content)
    if not ok then return nil end
    return val
end

function M.savefile(filepath, val, opts)
    local content = M.encode(val, opts)
    local f = io.open(filepath, "w")
    if not f then return false end
    f:write(content)
    f:write("\n")
    f:close()
    return true
end

return M
