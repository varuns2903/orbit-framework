# Load Testing & Benchmarks

These benchmarks demonstrate the performance of the **Orbit Framework** running a minimal server (`examples/benchmark_server.cpp`) on a single node.

*Testing environment: GitHub Actions / Local (Linux), ApacheBench.*

## 1. Plain Text Endpoint
- **Concurrency Level:** 100
- **Total Requests:** 10,000

```text
This is ApacheBench, Version 2.3 <$Revision: 1934973 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient)


Server Software:        
Server Hostname:        127.0.0.1
Server Port:            8888

Document Path:          /
Document Length:        13 bytes

Concurrency Level:      100
Time taken for tests:   0.461 seconds
Complete requests:      10000
Failed requests:        0
Keep-Alive requests:    10000
Total transferred:      1020000 bytes
HTML transferred:       130000 bytes
Requests per second:    21688.59 [#/sec] (mean)
Time per request:       4.611 [ms] (mean)
Time per request:       0.046 [ms] (mean, across all concurrent requests)
Transfer rate:          2160.39 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.7      0      10
Processing:     2    4   1.2      4      14
Waiting:        2    4   1.2      4      12
Total:          2    4   1.3      4      15

Percentage of the requests served within a certain time (ms)
  50%      4
  66%      4
  75%      4
  80%      5
  90%      5
  95%      7
  98%      8
  99%     10
 100%     15 (longest request)
```

## 2. JSON Endpoint
- **Concurrency Level:** 100
- **Total Requests:** 10,000

```text
This is ApacheBench, Version 2.3 <$Revision: 1934973 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient)


Server Software:        
Server Hostname:        127.0.0.1
Server Port:            8888

Document Path:          /json
Document Length:        27 bytes

Concurrency Level:      100
Time taken for tests:   0.530 seconds
Complete requests:      10000
Failed requests:        0
Keep-Alive requests:    10000
Total transferred:      1220000 bytes
HTML transferred:       270000 bytes
Requests per second:    18859.42 [#/sec] (mean)
Time per request:       5.302 [ms] (mean)
Time per request:       0.053 [ms] (mean, across all concurrent requests)
Transfer rate:          2246.92 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.3      0       4
Processing:     2    5   1.5      5      25
Waiting:        1    5   1.5      5      25
Total:          2    5   1.5      5      25

Percentage of the requests served within a certain time (ms)
  50%      5
  66%      5
  75%      6
  80%      6
  90%      6
  95%      8
  98%     10
  99%     11
 100%     25 (longest request)
```
