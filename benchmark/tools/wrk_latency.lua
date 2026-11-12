-- ===========================================================================
--  wrk_latency.lua — wrk script for the cppbalancer benchmark harness.
--
--  Extends wrk's default reporting with:
--    * an explicit latency percentile table (p50 / p75 / p90 / p99 / p99.9)
--    * a machine readable key=value block so the orchestrator can parse it
--    * a per-second request-rate time series (written per thread) so that
--      saturation / queueing behaviour is visible, not just the average
--
--  Usage:  wrk -t<threads> -c<conns> -d<dur> -s wrk_latency.lua http://host:port/
-- ===========================================================================

wrk.method  = "GET"
wrk.headers["Connection"] = "keep-alive"
wrk.headers["Accept"]     = "*/*"

local counter  = 1
local threads  = {}
local myid     = 0
local reqs     = 0
local tfile    = nil
local last_sec = 0
local sec_reqs = 0
local since_check = 0

function setup(thread)
    thread:set("id", counter)
    counter = counter + 1
end

function init(args)
    myid = tonumber(wrk.thread:get("id"))
    reqs = 0
    sec_reqs = 0
    since_check = 0
    last_sec = os.time()

    -- Per-thread time series file (one row per second).
    local path = os.getenv("WRK_TS_DIR")
    if path then
        local ok = pcall(function()
            tfile = assert(io.open(path .. "/ts_" .. myid .. ".csv", "w"))
            tfile:write("epoch,thread,requests,bytes\n")
        end)
        if not ok then tfile = nil end
    end
end

function request()
    reqs = reqs + 1
    sec_reqs = sec_reqs + 1
    return wrk.request()
end

-- Roll the per-second bucket over. Called every FLUSH_EVERY responses so
-- that os.time() is not invoked on the hot path (it would add a syscall per
-- request and distort the very numbers we are measuring).
local FLUSH_EVERY = 512

function response(status, headers, body)
    since_check = since_check + 1
    if since_check >= FLUSH_EVERY then
        since_check = 0
        flush_second()
    end
end

function flush_second()
    if not tfile then return end
    local now = os.time()
    if now > last_sec then
        tfile:write(string.format("%d,%d,%d,0\n", last_sec, myid, sec_reqs))
        last_sec = now
        sec_reqs = 0
    end
end

function done(summary, latency, requests)
    flush_second()
    if tfile then tfile:close() end

    local dur_s = summary.duration / 1000000.0
    local rps  = summary.requests / dur_s
    local bps  = summary.bytes / dur_s

    io.write("\n")
    io.write("=============== WRK RESULT ===============\n")
    io.write(string.format("requests_total   = %d\n", summary.requests))
    io.write(string.format("duration_s       = %.3f\n", dur_s))
    io.write(string.format("requests_per_sec = %.2f\n", rps))
    io.write(string.format("bytes_total      = %d\n", summary.bytes))
    io.write(string.format("bytes_per_sec    = %.2f\n", bps))
    io.write(string.format("transfer_MB_s    = %.3f\n", bps / (1024*1024)))
    io.write(string.format("socket_errors_connect = %d\n", summary.errors.connect))
    io.write(string.format("socket_errors_read    = %d\n", summary.errors.read))
    io.write(string.format("socket_errors_write   = %d\n", summary.errors.write))
    io.write(string.format("socket_errors_status  = %d\n", summary.errors.status))
    io.write(string.format("socket_errors_timeout = %d\n", summary.errors.timeout))
    io.write(string.format("non2xx_3xx = %d\n", summary.errors.status))
    io.write("--- latency_us ---\n")
    io.write(string.format("min    = %d\n", latency.min))
    io.write(string.format("mean   = %.2f\n", latency.mean))
    io.write(string.format("stdev  = %.2f\n", latency.stdev))
    io.write(string.format("max    = %d\n", latency.max))
    io.write(string.format("p50    = %d\n", latency:percentile(50)))
    io.write(string.format("p75    = %d\n", latency:percentile(75)))
    io.write(string.format("p90    = %d\n", latency:percentile(90)))
    io.write(string.format("p99    = %d\n", latency:percentile(99)))
    io.write(string.format("p99_9  = %d\n", latency:percentile(99.9)))
    io.write("--- threads ---\n")
    for _, t in ipairs(threads) do
        io.write(string.format("thread_%d_latency_us_p50 = %d\n", t, latency:percentile(50)))
        break
    end
    io.write("=========================================\n")
end
