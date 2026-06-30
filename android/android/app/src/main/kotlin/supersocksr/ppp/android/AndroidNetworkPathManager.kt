package supersocksr.ppp.android

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.VpnService
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.max
import kotlin.math.min

class AndroidNetworkPathManager(
    private val service: VpnService,
    private val policy: Policy,
    private val onSnapshot: (String) -> Unit,
    private val onUnderlyingNetworks: (List<Network>) -> Unit,
) {
    data class Policy(
        val pathAwarenessEnabled: Boolean,
        val multiNetworkEnabled: Boolean,
        val multiNetworkMode: String,
        val cellularPolicy: String,
        val primary: String,
    )

    private data class PathRecord(
        val id: Int,
        val network: Network,
        val networkHandle: String,
        var transport: String,
        var available: Boolean,
        var validated: Boolean,
        var metered: Boolean,
        var roaming: Boolean,
        var downstreamKbps: Int,
        var upstreamKbps: Int,
        var mtu: Int,
        var interfaceName: String,
        var multipathPreference: Int,
        var lastAvailableMs: Long,
        var lastLostMs: Long,
    )

    private val connectivityManager =
        service.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private val lock = Any()
    private val records = LinkedHashMap<Network, PathRecord>()
    private val idsByHandle = HashMap<String, Int>()
    private val callbacks = ArrayList<ConnectivityManager.NetworkCallback>()
    private var handlerThread: HandlerThread? = null
    private var sequence = 0
    private var nextId = 1
    private var started = false

    fun start() {
        if (!policy.pathAwarenessEnabled) {
            onSnapshot(disabledSnapshot(policy))
            onUnderlyingNetworks(emptyList())
            return
        }
        if (started) return
        started = true

        val thread = HandlerThread("openppp2-path-awareness").also { it.start() }
        handlerThread = thread
        val handler = Handler(thread.looper)

        seedCurrentNetworks()
        registerTransport(NetworkCapabilities.TRANSPORT_WIFI, "wifi", handler)
        registerTransport(NetworkCapabilities.TRANSPORT_CELLULAR, "cellular", handler)
        emit("start")
    }

    fun stop() {
        val snapshotCallbacks = synchronized(lock) {
            started = false
            ArrayList(callbacks).also { callbacks.clear() }
        }
        for (callback in snapshotCallbacks) {
            try {
                connectivityManager.unregisterNetworkCallback(callback)
            } catch (_: Throwable) {
            }
        }
        handlerThread?.quitSafely()
        handlerThread = null
    }

    fun flushUnderlyingNetworks() {
        val networks = synchronized(lock) { activeNetworksLocked() }
        onUnderlyingNetworks(networks)
    }

    private fun registerTransport(transport: Int, fallbackTransport: String, handler: Handler) {
        val request = NetworkRequest.Builder()
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN)
            .addTransportType(transport)
            .build()

        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                upsert(network, fallbackTransport, "available")
            }

            override fun onCapabilitiesChanged(
                network: Network,
                networkCapabilities: NetworkCapabilities,
            ) {
                upsert(network, fallbackTransport, "capabilities", networkCapabilities, null)
            }

            override fun onLinkPropertiesChanged(network: Network, linkProperties: LinkProperties) {
                upsert(network, fallbackTransport, "link", null, linkProperties)
            }

            override fun onLost(network: Network) {
                synchronized(lock) {
                    records[network]?.let {
                        it.available = false
                        it.lastLostMs = System.currentTimeMillis()
                    }
                }
                emit("lost")
            }

            override fun onUnavailable() {
                emit("unavailable")
            }
        }

        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                connectivityManager.registerNetworkCallback(request, callback, handler)
            } else {
                connectivityManager.registerNetworkCallback(request, callback)
            }
            callbacks.add(callback)
        } catch (e: Throwable) {
            PppLog.write(service, "path-awareness register $fallbackTransport failed", e)
        }
    }

    private fun seedCurrentNetworks() {
        try {
            for (network in connectivityManager.allNetworks) {
                val caps = connectivityManager.getNetworkCapabilities(network) ?: continue
                val transport = transportFromCapabilities(caps) ?: continue
                if (!caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) ||
                    !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN)
                ) {
                    continue
                }
                val link = connectivityManager.getLinkProperties(network)
                upsert(network, transport, "seed", caps, link)
            }
        } catch (e: Throwable) {
            PppLog.write(service, "path-awareness seed failed", e)
        }
    }

    private fun upsert(
        network: Network,
        fallbackTransport: String,
        reason: String,
        capabilities: NetworkCapabilities? = connectivityManager.getNetworkCapabilities(network),
        linkProperties: LinkProperties? = connectivityManager.getLinkProperties(network),
    ) {
        synchronized(lock) {
            val handle = networkHandle(network)
            val id = idsByHandle.getOrPut(handle) { nextId++ }
            val now = System.currentTimeMillis()
            val caps = capabilities
            val link = linkProperties
            val record = records[network] ?: PathRecord(
                id = id,
                network = network,
                networkHandle = handle,
                transport = fallbackTransport,
                available = true,
                validated = false,
                metered = true,
                roaming = false,
                downstreamKbps = 0,
                upstreamKbps = 0,
                mtu = 0,
                interfaceName = "",
                multipathPreference = 0,
                lastAvailableMs = now,
                lastLostMs = 0L,
            ).also { records[network] = it }

            record.available = true
            record.lastAvailableMs = now
            if (caps != null) {
                record.transport = transportFromCapabilities(caps) ?: fallbackTransport
                record.validated = caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)
                record.metered = !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED)
                record.roaming = !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_ROAMING)
                record.downstreamKbps = max(0, caps.linkDownstreamBandwidthKbps)
                record.upstreamKbps = max(0, caps.linkUpstreamBandwidthKbps)
                record.multipathPreference = multipathPreference(network)
            }
            if (link != null) {
                record.interfaceName = link.interfaceName ?: ""
                record.mtu = max(0, link.mtu)
            }
        }
        emit(reason)
    }

    private fun emit(reason: String) {
        val built = synchronized(lock) {
            sequence++
            val snapshot = buildSnapshotLocked(reason)
            val networks = activeNetworksLocked()
            val activeCount = records.values.count { it.available }
            Triple(snapshot, networks, activeCount)
        }
        onSnapshot(built.first)
        onUnderlyingNetworks(built.second)
        PppLog.write(service, "path-awareness reason=$reason active=${built.third}")
    }

    private fun buildSnapshotLocked(reason: String): String {
        val now = System.currentTimeMillis()
        val hasNonCellular = records.values.any { it.available && it.transport != "cellular" }
        val scored = records.values.map { it to score(it, hasNonCellular) }
        val primary = scored
            .filter { it.second > 0 && it.first.available }
            .maxByOrNull { it.second }
            ?.first

        val root = JSONObject()
            .put("schemaVersion", 1)
            .put("producer", "android-client")
            .put("sequence", sequence)
            .put("updatedAtMs", now)
            .put("reason", reason)
        root.put(
            "policy",
            JSONObject()
                .put("cellular", policy.cellularPolicy)
                .put("mode", if (policy.multiNetworkEnabled) policy.multiNetworkMode else "observe")
                .put("multiNetworkEnabled", policy.multiNetworkEnabled)
                .put("primary", policy.primary)
                .put("pathAwarenessEnabled", policy.pathAwarenessEnabled),
        )

        val paths = JSONArray()
        for ((record, score) in scored.sortedWith(compareByDescending<Pair<PathRecord, Int>> { it.second }.thenBy { it.first.id })) {
            paths.put(
                JSONObject()
                    .put("id", record.id)
                    .put("transport", record.transport)
                    .put("networkHandle", record.networkHandle)
                    .put("available", record.available)
                    .put("validated", record.validated)
                    .put("metered", record.metered)
                    .put("roaming", record.roaming)
                    .put("downstreamKbps", record.downstreamKbps)
                    .put("upstreamKbps", record.upstreamKbps)
                    .put("mtu", record.mtu)
                    .put("interfaceName", record.interfaceName)
                    .put("multipathPreference", record.multipathPreference)
                    .put("score", score)
                    .put("eligible", score > 0 && record.available)
                    .put("lastAvailableMs", record.lastAvailableMs)
                    .put("lastLostMs", record.lastLostMs),
            )
        }

        root.put(
            "local",
            JSONObject()
                .put("primaryPathId", primary?.id ?: JSONObject.NULL)
                .put("primaryTransport", primary?.transport ?: JSONObject.NULL)
                .put("paths", paths),
        )
        root.put("peer", JSONObject.NULL)
        return root.toString()
    }

    private fun score(record: PathRecord, hasNonCellular: Boolean): Int {
        if (!record.available) return 0
        if (!cellularAllowed(record, hasNonCellular)) return 0

        var score = 10
        if (record.validated) score += 35 else score += 10
        if (!record.metered) score += 20
        score += when (record.transport) {
            "wifi" -> 25
            "cellular" -> 15
            else -> 5
        }
        if (policy.primary != "auto") {
            score += if (record.transport == policy.primary) 15 else -10
        }
        score += min(15, record.downstreamKbps / 5000)
        if (record.roaming) score -= 15
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            if ((record.multipathPreference and ConnectivityManager.MULTIPATH_PREFERENCE_PERFORMANCE) != 0) {
                score += 5
            }
            if ((record.multipathPreference and ConnectivityManager.MULTIPATH_PREFERENCE_RELIABILITY) != 0) {
                score += 4
            }
            if ((record.multipathPreference and ConnectivityManager.MULTIPATH_PREFERENCE_HANDOVER) != 0) {
                score += 2
            }
        }
        return min(100, max(0, score))
    }

    private fun cellularAllowed(record: PathRecord, hasNonCellular: Boolean): Boolean {
        if (record.transport != "cellular") return true
        return when (policy.cellularPolicy) {
            "never" -> false
            "always" -> true
            else -> {
                if (!hasNonCellular) {
                    true
                } else {
                    !record.metered || record.multipathPreference != 0
                }
            }
        }
    }

    private fun activeNetworksLocked(): List<Network> {
        if (!policy.multiNetworkEnabled) return emptyList()

        val active = records.values
            .filter { it.available && score(it, records.values.any { r -> r.available && r.transport != "cellular" }) > 0 }
            .sortedByDescending { score(it, records.values.any { r -> r.available && r.transport != "cellular" }) }
        val selected = if (policy.multiNetworkMode == "parallel") active else active.take(1)
        return selected.map { it.network }
    }

    private fun multipathPreference(network: Network): Int {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return 0
        return try {
            connectivityManager.getMultipathPreference(network)
        } catch (_: Throwable) {
            0
        }
    }

    private fun networkHandle(network: Network): String {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            network.networkHandle.toString()
        } else {
            network.toString()
        }
    }

    private fun transportFromCapabilities(caps: NetworkCapabilities): String? {
        return when {
            caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> "wifi"
            caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) -> "cellular"
            else -> null
        }
    }

    companion object {
        fun disabledSnapshot(policy: Policy): String {
            val now = System.currentTimeMillis()
            return JSONObject()
                .put("schemaVersion", 1)
                .put("producer", "android-client")
                .put("sequence", 0)
                .put("updatedAtMs", now)
                .put("reason", "disabled")
                .put(
                    "policy",
                    JSONObject()
                        .put("cellular", policy.cellularPolicy)
                        .put("mode", "observe")
                        .put("multiNetworkEnabled", policy.multiNetworkEnabled)
                        .put("primary", policy.primary)
                        .put("pathAwarenessEnabled", false),
                )
                .put(
                    "local",
                    JSONObject()
                        .put("primaryPathId", JSONObject.NULL)
                        .put("primaryTransport", JSONObject.NULL)
                        .put("paths", JSONArray()),
                )
                .put("peer", JSONObject.NULL)
                .toString()
        }
    }
}
