// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.videomotion

import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.res.Configuration
import android.graphics.drawable.Drawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.TextWatcher
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import androidx.preference.Preference
import androidx.preference.PreferenceCategory
import androidx.preference.PreferenceGroupAdapter
import androidx.preference.PreferenceViewHolder
import androidx.recyclerview.widget.LinearLayoutManager
import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity
import com.android.settingslib.widget.AppSwitchPreference
import com.android.settingslib.widget.FooterPreference
import com.android.settingslib.widget.GroupSectionDividerMixin
import com.android.settingslib.widget.MainSwitchPreference
import com.android.settingslib.widget.SettingsBasePreferenceFragment
import com.android.settingslib.widget.category.R as CategoryR
import com.android.settingslib.widget.preference.searchbox.R as SearchBoxR
import com.android.settingslib.widget.theme.R as ThemeR
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
    private lateinit var footer: FooterPreference
    private var searchFocused = false
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
                    if (searchFocused) revealSearch()
                }
                onFocusChanged = {
                    searchFocused = it
                    if (it) revealSearch()
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
        footer =
            FooterPreference(context).apply {
                title = listOf(R.string.limits, R.string.shared_uid, R.string.user_limit)
                    .joinToString("\n\n") { getString(it) }
            }
        preferenceScreen.addPreference(footer)
        updateAppAvailability()
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        val list = listView ?: return
        // The keyboard shrinks the list; keep the search field at the top while it is focused.
        list.addOnLayoutChangeListener { _, _, top, _, bottom, _, oldTop, _, oldBottom ->
            if (searchFocused && bottom - top != oldBottom - oldTop) revealSearch()
        }
    }

    // In landscape the intro text and the switch would fill the space above the keyboard, so the
    // search field is scrolled to the top and the large title is collapsed.
    private fun revealSearch() {
        if (resources.configuration.orientation != Configuration.ORIENTATION_LANDSCAPE) return
        (activity as? CollapsingToolbarBaseActivity)?.appBarLayout?.setExpanded(false, true)
        val list = listView ?: return
        list.post {
            val position = (list.adapter as? PreferenceGroupAdapter)?.getPreferenceAdapterPosition(appSearch)
            if (position != null && position >= 0) {
                (list.layoutManager as? LinearLayoutManager)?.scrollToPositionWithOffset(position, 0)
            }
        }
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
        // A search left in a disabled list would hide the other apps with no way to clear it.
        if (!enabled) appSearch.clear()
    }

    private fun filterApps() {
        appGroup.removeAll()
        val selected = MotionController.selected()
        // Apps that are on come first; the stable sort keeps the locale order inside both groups.
        val matches =
            apps
                .filter {
                    it.label.contains(query.trim(), ignoreCase = true) ||
                        it.packageName.contains(query.trim(), ignoreCase = true)
                }
                .sortedByDescending { it.packageName in selected }
        for (app in matches) {
            appGroup.addPreference(
                AppRowPreference(requireContext()).apply {
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
                EmptyPreference(requireContext()).apply { setTitle(R.string.no_apps) }
            )
        }
        // The notes stay for an empty list that is not a search result, such as a non-admin user.
        footer.isVisible = matches.isNotEmpty() || apps.isEmpty()
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
 * Search field that lives in the preference list, so it scrolls with the apps it filters. It looks
 * like the SettingsLib expressive search box, which SearchBoxPreference itself only reports on the
 * IME search action; this one reports every text change and has a 48dp clear button.
 */
private class AppSearchPreference(context: Context) :
    Preference(context), GroupSectionDividerMixin {
    var onQueryChanged: ((String) -> Unit)? = null
    var onFocusChanged: ((Boolean) -> Unit)? = null
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
        layoutResource = R.layout.app_search
        setIcon(SearchBoxR.drawable.settingslib_expressive_searchbox_search_icon_24dp)
        isSelectable = false
        isPersistent = false
    }

    fun setQuery(value: String) {
        query = value
    }

    /** Empties the field and reports it like typing would. */
    fun clear() {
        if (query.isEmpty()) return
        val edit = editText
        if (edit != null) {
            edit.setText("")
        } else {
            query = ""
            onQueryChanged?.invoke("")
        }
    }

    private fun updateClear(text: CharSequence?) {
        clearButton?.visibility = if (text.isNullOrEmpty()) View.GONE else View.VISIBLE
    }

    override fun onBindViewHolder(holder: PreferenceViewHolder) {
        super.onBindViewHolder(holder)
        editText?.removeTextChangedListener(watcher)
        val edit = holder.findViewById(R.id.search_query) as EditText
        editText = edit
        clearButton = holder.findViewById(R.id.clear_button)
        if (edit.text.toString() != query) edit.setText(query)
        updateClear(query)
        holder.findViewById(R.id.searchbox_container)?.setOnClickListener {
            edit.requestFocus()
            context.getSystemService(InputMethodManager::class.java)?.showSoftInput(edit, 0)
        }
        clearButton?.setOnClickListener { edit.setText("") }
        edit.setOnFocusChangeListener { _, focused -> onFocusChanged?.invoke(focused) }
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

/** An app row that follows the app list spec: 64dp tall, 16dp icon gap. */
private class AppRowPreference(context: Context) : AppSwitchPreference(context) {
    override fun onBindViewHolder(holder: PreferenceViewHolder) {
        super.onBindViewHolder(holder)
        val density = context.resources.displayMetrics.density
        val height = Math.round(ROW_HEIGHT_DP * density)
        // The root and its first child both carry the 72dp minimum of the expressive row layout.
        holder.itemView.minimumHeight = height
        (holder.itemView as? ViewGroup)?.getChildAt(0)?.minimumHeight = height
        (holder.findViewById(ThemeR.id.icon_frame)?.layoutParams as? ViewGroup.MarginLayoutParams)?.marginEnd =
            Math.round(ICON_GAP_DP * density)
    }

    private companion object {
        const val ROW_HEIGHT_DP = 64
        const val ICON_GAP_DP = 16
    }
}

/** Centred note for an empty list. It is a divider, so it draws without a card behind it. */
private class EmptyPreference(context: Context) : Preference(context), GroupSectionDividerMixin {
    init {
        layoutResource = R.layout.app_empty
        isSelectable = false
        isPersistent = false
    }
}
