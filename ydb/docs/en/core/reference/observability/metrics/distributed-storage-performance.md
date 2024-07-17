# Distributed Storage performance metrics

Distributed storage has a specific throughput limited by the resources of physical devices in the cluster and can provide low response times if the load does not exceed this capacity. Performance metrics display the amount of available resources of physical devices and allow for assessing of their consumption level. Tracking the values of performance metrics allows to monitor whether the necessary conditions for low response time guarantees are met, specifically, that the average load does not exceed the available limit and that there are no short-term load bursts.

### Request cost model

The request cost is an estimate of the time that a physical device spends performing a given operation. The request cost is calculated using a simple physical device model. It assumes that the physical device can only handle one request for reading or writing at a time. The operation execution takes a certain amount of the device’s working time; therefore, the total time spent on requests over a certain period cannot exceed the duration of that period.

The request cost is calculated using a linear formula:

$$
cost(operation) = A + operation.size() \times B
$$

The physical rationale behind the linear dependency is as follows: coefficient $A$ is the time needed for the physical device to access the data, and coefficient $B$ is the time required to read or write one byte of data.

The coefficients $A$ and $B$ depend on the request request and device type. These coefficients were measured experimentally for each device type and each request type.

In {{ ydb-short-name }}, all physical devices are divided into three types: HDD, SATA SSD (further referred to as SSD), and NVMe SSD (further referred to as NVMe). HDDs are rotating hard drives characterized by high data access time. SSD and NVMe types differ in their interfaces: NVMe provides a higher operation speed.

Operations are divided into three types: reads, writes, and huge-writes. The division of writes into regular and huge-writes is due to the specifics of handling write requests on VDisks.

In addition to user requests, the load on distributed storage is created by background processes of compaction, scrubbing, and defragmentation, as well as internal communication between VDisks. The compaction process can create particularly high loads when there is a substantial flow of small blob writes.

### Available disk time {#diskTimeAvailable}

The PDisk scheduler manages the requests execution order from its client VDisks. PDisk fairly divides the device's time among its VDisks, ensuring that each of the $n$ VDisks is guaranteed $1/n$ seconds of the physical device's working time each second. Based on the information about the number of neighboring VDisks for each VDisk, denoted as $N$, and the configurable parameter `DiskTimeAvailableScale`, the available disk time estimate, referred to as `DiskTimeAvailable`, is calculated by the formula:
$$
    DiskTimeAvailable = \dfrac{1000000000}{N} \cdot \dfrac{DiskTimeAvailableScale}{1000}
$$

### Load burst detector {#burstDetector}

A burst is a sharp, short-term increase in the load on a VDisk, which can lead to degradation in the response time of operations. The values of sensors on cluster nodes are collected at certain intervals, for example, every 15 seconds, making it impossible to reliably detect short-term events using only the metrics of request cost and available disk time. A modified [Token Bucket algorithm](https://en.wikipedia.org/wiki/Token_bucket) is used to address this issue. In this modification, the bucket can have a negative number of tokens and such a state is called underflow. A separate Token Bucket object is associated with each VDisk. The minimum expected response time, at which an increase in load is considered a burst, is determined by the configurable parameter `BurstThresholdNs`. The bucket will underflow if the calculated time needed to process the requests in nanoseconds exceeds the `BurstThresholdNs` value.

### Performance metrics

Performance metrics are calculated based on the following VDisk sensors:
| Sensor Name           | Units             | Description                                                                           |
|-----------------------|-------------------|---------------------------------------------------------------------------------------|
| `DiskTimeAvailable`   | arbitrary units   | Available disk time. |
| `UserDiskCost`        | arbitrary units   | Total cost of requests a VDisk receives from the DS Proxy.                            |
| `InternalDiskCost`    | arbitrary units   | Total cost of requests received by a VDisk from another VDisk in the group, for example, as part of the replication process. |
| `CompactionDiskCost`  | arbitrary units   | Total cost of requests the VDisk sends as part of the compaction process.             |
| `DefragDiskCost`      | arbitrary units   | Total cost of requests the VDisk sends as part of the defragmentation process.        |
| `ScrubDiskCost`       | arbitrary units   | Total cost of requests the VDisk sends as part of the scrubbing process.              |
| `BurstDetector_redMs` | ms                | The duration in milliseconds during which the Token Bucket was in an underflow state. |

`DiskTimeAvailable` and the request cost are estimates of available and consumed bandwidth, respectively, and not actually measured time, therefore both of these quantities are measured in arbitrary units.

### Conditions for Distributed Storage guarantees {#requirements}

The {{ ydb-short-name }} distributed storage can ensure low response times only under the following conditions:

1. $DiskTimeAvailable >= UserDiskCost + InternalDiskCost + CompactionDiskCost + DefragDiskCost + ScrubDiskCost$ — The average load does not exceed the maximum allowed.
2. $BurstDetector_redMs = 0$ — There are no short-term load bursts, which would lead to request queues on handlers.

### Performance metrics configuration

Since the coefficients for the request cost formula were measured on specific physical devices from development clusters, and the performance of other devices may vary, the metrics may require additional adjustments to be used as a source of guarantees for Distributed Storage. Performance metric parameters can be managed via [dynamic cluster configuration](../../../maintenance/manual/dynamic-config.md) and the Immediate Controls mechanism without restarting {{ ydb-short-name }} processes.

| Parameter Name                        | Description                                                                                   | Default Value |
|---------------------------------------|-----------------------------------------------------------------------------------------------|---------------|
| `disk_time_available_scale_hdd`       | [`DiskTimeAvailableScale` parameter](#diskTimeAvailable) for VDisks running on HDD devices.   | `1000`        |
| `disk_time_available_scale_ssd`       | [`DiskTimeAvailableScale` parameter](#diskTimeAvailable) for VDisks running on SSD devices.   | `1000`        |
| `disk_time_available_partition_nvme`  | [`DiskTimeAvailableScale` parameter](#diskTimeAvailable) for VDisks running on NVMe devices.  | `1000`        |
| `burst_threshold_ns_hdd`              | [`BurstThresholdNs` parameter](#burstDetector) for VDisks running on HDD devices.             | `200000000`   |
| `burst_threshold_ns_ssd`              | [`BurstThresholdNs` parameter](#burstDetector) for VDisks running on SSD devices.             | `50000000`    |
| `burst_threshold_ns_nvme`             | [`BurstThresholdNs` parameter](#burstDetector) for VDisks running on NVMe devices.            | `32000000`    |

#### Configuration examples

If a given {{ ydb-short-name }} cluster uses NVMe devices and delivers performance that is 10% higher than the baseline, add the following section to the `immediate_controls_config` in the dynamic configuration of the cluster:

```
vdisk_controls:
  disk_time_available_scale_nvme: 1100
```

If a given {{ ydb-short-name }} cluster is using HDD devices and under its workload conditions, the maximum tolerable response time is 500 ms, add the following section to the `immediate_controls_config` in the dynamic configuration of the cluster:

```
vdisk_controls:
  burst_threshold_ns_hdd: 500000000
```

### How to compare the performance of a cluster with the baseline

To compare the performance of Distributed Storage in a cluster with the baseline, you need to load the distributed storage with requests to the point where the VDisks cannot process the incoming request flow. At this moment, requests start to queue up, and the response time of the VDisks increases sharply. Compute the value $D$ just before the overload:
$$
D = \frac{UserDiskCost + InternalDiskCost + CompactionDiskCost + DefragDiskCost + ScrubDiskCost}{DiskTimeAvailable}
$$
Set the `disk_time_available_scale_<used-device-type>` configuration parameter equal to the calculated value of $D$, multiplied by 1000 and rounded. We assume that the physical devices in the user cluster are comparable in performance to the baseline; hence, by default, the `disk_time_available_scale_<used-device-type>` parameter is set to 1000.

Such a load can be created, for example, using [Storage LoadActor](../../../contributor/load-actors-storage.md).

