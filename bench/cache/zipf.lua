-- wrk script: GET <prefix>/bench/fNNN.txt with a Zipf distribution (s = 1).
-- Rank 1 is f000. With 200 files, the top 20% of files get about 70% of
-- the requests. Each thread has a fixed seed, so each run asks for the
-- same sequence. response() counts the X-Cache header; done() prints the
-- sum of all threads. The first script argument is the prefix: "/proxy"
-- for the Proxy Route of the server, "" for rproxy.
local FILE_COUNT = 200
local EXPONENT = 1.0
local threads = {}

function setup(thread)
    thread:set("thread_id", #threads + 1)
    table.insert(threads, thread)
end

function init(args)
    prefix = args[1] or "/proxy"
    math.randomseed(42 + thread_id)
    cdf = {}
    local total = 0
    for rank = 1, FILE_COUNT do
        total = total + 1 / rank ^ EXPONENT
        cdf[rank] = total
    end
    for rank = 1, FILE_COUNT do
        cdf[rank] = cdf[rank] / total
    end
    hits = 0
    misses = 0
end

function request()
    local pick = math.random()
    local low, high = 1, FILE_COUNT
    while low < high do
        local middle = math.floor((low + high) / 2)
        if cdf[middle] < pick then
            low = middle + 1
        else
            high = middle
        end
    end
    return wrk.format("GET", string.format("%s/bench/f%03d.txt", prefix, low - 1))
end

function response(status, headers, body)
    local state = headers["X-Cache"]
    if state == "HIT" then
        hits = hits + 1
    elseif state == "MISS" then
        misses = misses + 1
    end
end

function done(summary, latency, requests)
    local hit_sum, miss_sum = 0, 0
    for _, thread in ipairs(threads) do
        hit_sum = hit_sum + thread:get("hits")
        miss_sum = miss_sum + thread:get("misses")
    end
    io.write(string.format("xcache hits %d misses %d\n", hit_sum, miss_sum))
end
