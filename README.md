# 一、项目启动

在mywebserver文件夹下

1.编译

```
g++ *.cpp -pthread
```

2.运行

```
./a.out 10000
```

二、压力测试

在test_presure/webbench-1.5/文件夹下

1.编译

```
make
```

2.运行

```
./webbench
./webbench -c 7000 -t 5 http://192.168.136.128:10000/index.html
```

