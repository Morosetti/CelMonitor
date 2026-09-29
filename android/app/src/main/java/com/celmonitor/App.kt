package com.celmonitor

import android.app.Application
import com.celmonitor.session.ConnectionManager

class App : Application() {
    lateinit var connections: ConnectionManager
        private set

    override fun onCreate() {
        super.onCreate()
        connections = ConnectionManager(this)
    }
}
