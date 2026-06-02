package io.neoterm.ui.customize

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.MenuItem
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Spinner
import android.widget.Toast
import io.neoterm.R
import io.neoterm.component.ComponentManager
import io.neoterm.component.colorscheme.ColorSchemeComponent
import io.neoterm.component.config.NeoTermPath
import io.neoterm.component.font.FontComponent
import java.io.File

/**
 * @author kiva
 */
class CustomizeActivity : BaseCustomizeActivity() {
  private val REQUEST_SELECT_FONT = 22222
  private val REQUEST_SELECT_COLOR = 22223

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    initCustomizationComponent(R.layout.ui_customize)

    findViewById<View>(R.id.custom_install_font_button).setOnClickListener {
      val intent = Intent()
      intent.action = Intent.ACTION_GET_CONTENT
      intent.type = "*/*"
      startActivityForResult(Intent.createChooser(intent, getString(R.string.install_font)), REQUEST_SELECT_FONT)
    }

    findViewById<View>(R.id.custom_install_color_button).setOnClickListener {
      val intent = Intent()
      intent.action = Intent.ACTION_GET_CONTENT
      intent.type = "*/*"
      startActivityForResult(
        Intent.createChooser(intent, getString(R.string.install_color)),
        REQUEST_SELECT_COLOR
      )
    }
  }

  private fun setupSpinners() {
    val fontComponent = ComponentManager.getComponent<FontComponent>()
    val colorSchemeComponent = ComponentManager.getComponent<ColorSchemeComponent>()

    setupSpinner(R.id.custom_font_spinner, fontComponent.getFontNames(),
      fontComponent.getCurrentFontName(), object : AdapterView.OnItemSelectedListener {
        override fun onNothingSelected(parent: AdapterView<*>?) {
        }

        override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
          val fontName = parent!!.adapter!!.getItem(position) as String
          val font = fontComponent.getFont(fontName)
          fontComponent.applyFont(terminalView, extraKeysView, font)
          fontComponent.setCurrentFont(fontName)
        }
      })

    val colorData = listOf(
      getString(R.string.new_color_scheme),
      *colorSchemeComponent.getColorSchemeNames().toTypedArray()
    )
    setupSpinner(R.id.custom_color_spinner, colorData,
      colorSchemeComponent.getCurrentColorSchemeName(), object : AdapterView.OnItemSelectedListener {
        override fun onNothingSelected(parent: AdapterView<*>?) {
        }

        override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
          if (position == 0) {
            val intent = Intent(this@CustomizeActivity, ColorSchemeActivity::class.java)
            startActivity(intent)
            return
          }
          val colorName = parent!!.adapter!!.getItem(position) as String
          val color = colorSchemeComponent.getColorScheme(colorName)
          colorSchemeComponent.applyColorScheme(terminalView, extraKeysView, color)
          colorSchemeComponent.setCurrentColorScheme(colorName)
        }
      })
  }

  private fun setupSpinner(
    id: Int,
    data: List<String>,
    selected: String,
    listener: AdapterView.OnItemSelectedListener
  ): Spinner {
    val spinner = findViewById<Spinner>(id)
    val adapter = ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, data)
    adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
    spinner.adapter = adapter
    spinner.onItemSelectedListener = listener
    spinner.setSelection(if (data.contains(selected)) data.indexOf(selected) else 0)
    return spinner
  }

  override fun onResume() {
    super.onResume()
    // Re-scan so schemes/fonts added elsewhere (the new-scheme editor or an
    // import) appear without leaving and reopening the screen.
    ComponentManager.getComponent<ColorSchemeComponent>().reloadColorSchemes()
    ComponentManager.getComponent<FontComponent>().reloadFonts()
    setupSpinners()
  }

  override fun onDestroy() {
    super.onDestroy()
    session.finishIfRunning()
  }

  override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
    if (resultCode == RESULT_OK && data != null) {
      val uri = data.data
      if (uri != null) {
        when (requestCode) {
          REQUEST_SELECT_FONT -> installFont(uri)
          REQUEST_SELECT_COLOR -> installColor(uri)
        }
      }
    }
    super.onActivityResult(requestCode, resultCode, data)
  }

  private fun installColor(uri: Uri) {
    if (installFromUri(uri, NeoTermPath.COLORS_PATH)) {
      // Re-scan the colors directory so the freshly imported scheme shows up.
      ComponentManager.getComponent<ColorSchemeComponent>().reloadColorSchemes()
      setupSpinners()
    }
  }

  private fun installFont(uri: Uri) {
    if (installFromUri(uri, NeoTermPath.FONT_PATH)) {
      // Re-scan the fonts directory so the freshly imported font shows up.
      ComponentManager.getComponent<FontComponent>().reloadFonts()
      setupSpinners()
    }
  }

  /**
   * Copy the picked document straight from its content URI into [targetDir].
   * Reading from the stream (instead of resolving the URI to a file path) works
   * under scoped storage / the Storage Access Framework, where a real path is
   * usually not available.
   */
  private fun installFromUri(uri: Uri, targetDir: String): Boolean {
    return kotlin.runCatching {
      val name = queryDisplayName(uri) ?: uri.lastPathSegment?.substringAfterLast('/')
      ?: throw IllegalStateException("Cannot determine file name")
      val dir = File(targetDir).apply { mkdirs() }
      val target = File(dir, name)
      contentResolver.openInputStream(uri)?.use { input ->
        target.outputStream().use { output -> input.copyTo(output) }
      } ?: throw java.io.IOException("Cannot open $uri")
      true
    }.onFailure {
      Toast.makeText(this, getString(R.string.error) + ": ${it.localizedMessage}", Toast.LENGTH_LONG).show()
    }.getOrDefault(false)
  }

  private fun queryDisplayName(uri: Uri): String? {
    if (uri.scheme == "file") return uri.lastPathSegment
    return contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
      if (it.moveToFirst()) {
        val idx = it.getColumnIndex(OpenableColumns.DISPLAY_NAME)
        if (idx >= 0) it.getString(idx) else null
      } else null
    }
  }

  override fun onOptionsItemSelected(item: MenuItem): Boolean {
    when (item.itemId) {
      android.R.id.home -> finish()
    }
    return super.onOptionsItemSelected(item)
  }
}
