// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.videomotion

import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.graphics.drawable.Drawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.TextWatcher
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import androidx.preference.Preference
import androidx.preference.PreferenceCategory
import androidx.preference.PreferenceViewHolder
import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity
import com.android.settingslib.widget.AppSwitchPreference
import com.android.settingslib.widget.FooterPreference
import com.android.settingslib.widget.GroupSectionDividerMixin
import com.android.settingslib.widget.MainSwitchPreference
import com.android.settingslib.widget.SettingsBasePreferenceFragment
import com.android.settingslib.widget.TopIntroPreference
import com.android.settingslib.widget.category.R as CategoryR
import com.android.settingslib.widget.preference.searchbox.R as SearchBoxR
import java.text.Collator
import java.util.concurrent.Executors

class SettingsActivity : CollapsingToolbarBaseActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val frame = com.android.settingslib.collapsingtoolbar.R.id.content_frame
        if (supportFragmentManager.findFragmentById(frame) == null) {
            supportFragmentManager.beginTransaction().add(frame, SettingsFragment()).commit()
        }
    }
}

class SettingsFragment : SettingsBasePreferenceFragment() {
    private data class AppEntry(val packageName: String, val label: String, val icon: Drawable)

    private var apps = emptyList<AppEntry>()
    private var query = ""
    private lateinit var master: MainSwitchPreference
    private lateinit var appSearch: AppSearchPreference
    private lateinit var appGroup: PreferenceCategory
    private val loader = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    private var generation = 0
    private val listener =
        SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (key == MotionController.ENABLED) {
                master.isChecked = MotionController.enabled()
                updateAppAvailability()
            }
            if (key == "selected_packages") updateChecks()
        }

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        preferenceManager.setStorageDeviceProtected()
        preferenceManager.sharedPreferencesName = "video_motion"
        query = savedInstanceState?.getString(STATE_QUERY).orEmpty()
        val context = requireContext()
        preferenceScreen = preferenceManager.createPreferenceScreen(context)
        preferenceScreen.addPreference(
            TopIntroPreference(context).apply {
                setTitle(
                    if (MotionController.isOwner()) R.string.availability else R.string.user_limit
                )
            }
        )
        master =
            MainSwitchPreference(context).apply {
                key = MotionController.ENABLED
                setTitle(R.string.enabled)
                isPersistent = false
                isChecked = MotionController.enabled()
                isEnabled = MotionController.isOwner()
                setOnPreferenceChangeListener { _, value ->
                    MotionController.setEnabled(value as Boolean)
                    updateAppAvailability()
                    true
                }
            }
        preferenceScreen.addPreference(master)
        appSearch =
            AppSearchPreference(context).apply {
                setQuery(this@SettingsFragment.query)
                onQueryChanged = {
                    this@SettingsFragment.query = it
                    filterApps()
                }
            }
        preferenceScreen.addPreference(appSearch)
        appGroup =
            PreferenceCategory(context).apply {
                isOrderingAsAdded = true
                layoutResource =
                    CategoryR.layout.settingslib_expressive_untitled_preference_category
            }
        preferenceScreen.addPreference(appGroup)
        updateAppAvailability()
        preferenceScreen.addPreference(
            FooterPreference(context).apply { setSummary(R.string.shared_uid) }
        )
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(STATE_QUERY, query)
    }

    override fun onStart() {
        super.onStart()
        MotionController.preferences.registerOnSharedPreferenceChangeListener(listener)
        master.isChecked = MotionController.enabled()
        updateAppAvailability()
        if (!MotionController.isOwner()) return
        val pm = requireContext().packageManager
        val ownPackage = requireContext().packageName
        val request = ++generation
        loader.execute {
            val intent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
            val collator = Collator.getInstance()
            val entries =
                pm.queryIntentActivities(intent, 0)
                    .distinctBy { it.activityInfo.packageName }
                    .filter { it.activityInfo.packageName != ownPackage }
                    .map {
                        AppEntry(
                            it.activityInfo.packageName,
                            it.loadLabel(pm).toString(),
                            it.loadIcon(pm),
                        )
                    }
                    .sortedWith { a, b -> collator.compare(a.label, b.label) }
            main.post {
                if (isAdded && view != null && request == generation) {
                    apps = entries
                    filterApps()
                }
            }
        }
    }

    override fun onStop() {
        generation++
        MotionController.preferences.unregisterOnSharedPreferenceChangeListener(listener)
        super.onStop()
    }

    override fun onDestroy() {
        loader.shutdownNow()
        main.removeCallbacksAndMessages(null)
        super.onDestroy()
    }

    private fun updateAppAvailability() {
        val enabled = MotionController.isOwner() && MotionController.enabled()
        appSearch.isEnabled = enabled
        appGroup.isEnabled = enabled
    }

    private fun filterApps() {
        appGroup.removeAll()
        val selected = MotionController.selected()
        val matches =
            apps.filter {
                it.label.contains(query.trim(), ignoreCase = true) ||
                    it.packageName.contains(query.trim(), ignoreCase = true)
            }
        for (app in matches) {
            appGroup.addPreference(
                AppSwitchPreference(requireContext()).apply {
                    key = app.packageName
                    title = app.label
                    icon = app.icon
                    isPersistent = false
                    isChecked = app.packageName in selected
                    setOnPreferenceChangeListener { _, value ->
                        MotionController.setSelected(app.packageName, value as Boolean)
                        true
                    }
                }
            )
        }
        if (matches.isEmpty()) {
            appGroup.addPreference(
                Preference(requireContext()).apply {
                    setTitle(R.string.no_apps)
                    isSelectable = false
                }
            )
        }
    }

    private fun updateChecks() {
        val selected = MotionController.selected()
        for (i in 0 until appGroup.preferenceCount) {
            (appGroup.getPreference(i) as? AppSwitchPreference)?.let {
                it.isChecked = it.key in selected
            }
        }
    }
}

private const val STATE_QUERY = "query"

/**
 * Search field that lives in the preference list, so it scrolls with the apps it filters. It uses
 * the SettingsLib expressive search box layout, which SearchBoxPreference itself only reports on
 * the IME search action; this one reports every text change.
 */
private class AppSearchPreference(context: Context) :
    Preference(context), GroupSectionDividerMixin {
    var onQueryChanged: ((String) -> Unit)? = null
    private var query = ""
    private var editText: EditText? = null
    private val watcher =
        object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}

            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}

            override fun afterTextChanged(s: Editable?) {
                val text = s.toString()
                updateClear(text)
                if (text != query) {
                    query = text
                    onQueryChanged?.invoke(text)
                }
            }
        }
    private var clearButton: View? = null

    init {
        layoutResource = SearchBoxR.layout.settingslib_expressive_preference_searchbox
        setIcon(SearchBoxR.drawable.settingslib_expressive_searchbox_search_icon_24dp)
        isSelectable = false
        isPersistent = false
    }

    fun setQuery(value: String) {
        query = value
    }

    private fun updateClear(text: CharSequence?) {
        clearButton?.visibility = if (text.isNullOrEmpty()) View.GONE else View.VISIBLE
    }

    override fun onBindViewHolder(holder: PreferenceViewHolder) {
        super.onBindViewHolder(holder)
        editText?.removeTextChangedListener(watcher)
        val edit = holder.findViewById(SearchBoxR.id.search_query) as EditText
        editText = edit
        clearButton = holder.findViewById(SearchBoxR.id.clear_button)
        edit.setHint(R.string.search_apps)
        if (edit.text.toString() != query) edit.setText(query)
        updateClear(query)
        holder.findViewById(SearchBoxR.id.searchbox_container)?.setOnClickListener {
            edit.requestFocus()
            context.getSystemService(InputMethodManager::class.java)?.showSoftInput(edit, 0)
        }
        clearButton?.setOnClickListener { edit.setText("") }
        edit.setOnEditorActionListener { view, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_SEARCH) {
                context
                    .getSystemService(InputMethodManager::class.java)
                    ?.hideSoftInputFromWindow(view.windowToken, 0)
                true
            } else {
                false
            }
        }
        edit.addTextChangedListener(watcher)
    }
}
