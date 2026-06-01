package com.github.wrdlbrnft.sortedlistadapter

import android.content.Context
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import androidx.recyclerview.widget.SortedList

/**
 * A wrdlbrnft/SortedListAdapter könyvtár minimális, API-kompatibilis
 * újraimplementációja.
 *
 * Az eredeti artefaktumok (`com.github.wrdlbrnft:sorted-list-adapter` és a
 * `modular-adapter` függősége) kizárólag a megszűnt jcenter/bintray-en
 * léteztek, git tag nélkül — így egyetlen élő Maven-repóból sem oldhatók fel.
 * Hogy a NeoTerm build végleg self-contained és determinisztikus legyen, a
 * ténylegesen használt felületet (ViewModel, ViewHolder, ComparatorBuilder,
 * Callback, edit()/add()/replaceAll()/commit()) az androidx [SortedList]-re
 * építve hozzuk magunkkal, az eredeti csomagnéven — így a hívó kód
 * (`io.neoterm.ui.pm`, `io.neoterm.ui.customize`) változtatás nélkül fordul.
 */
abstract class SortedListAdapter<T : SortedListAdapter.ViewModel>(
  context: Context,
  itemClass: Class<T>,
  private val comparator: Comparator<T>
) : RecyclerView.Adapter<SortedListAdapter.ViewHolder<*>>() {

  /** A listában tárolt elemek által megvalósítandó interfész. */
  interface ViewModel {
    fun <M> isSameModelAs(model: M): Boolean
    fun <M> isContentTheSameAs(model: M): Boolean
  }

  /** A RecyclerView nézet-tartója; a kötést a [performBind] végzi. */
  abstract class ViewHolder<M : ViewModel>(itemView: View) : RecyclerView.ViewHolder(itemView) {
    fun bind(item: M) = performBind(item)
    protected abstract fun performBind(item: M)
  }

  /** Szerkesztési életciklus-visszahívások (animációkhoz). */
  interface Callback {
    fun onEditStarted()
    fun onEditFinished()
  }

  private val callbacks = mutableListOf<Callback>()

  private val sortedList: SortedList<T> = SortedList(
    itemClass,
    object : SortedList.Callback<T>() {
      override fun compare(a: T, b: T): Int = comparator.compare(a, b)
      override fun onInserted(position: Int, count: Int) = notifyItemRangeInserted(position, count)
      override fun onRemoved(position: Int, count: Int) = notifyItemRangeRemoved(position, count)
      override fun onMoved(fromPosition: Int, toPosition: Int) = notifyItemMoved(fromPosition, toPosition)
      override fun onChanged(position: Int, count: Int) = notifyItemRangeChanged(position, count)
      override fun areContentsTheSame(oldItem: T, newItem: T): Boolean = oldItem.isContentTheSameAs(newItem)
      override fun areItemsTheSame(item1: T, item2: T): Boolean = item1.isSameModelAs(item2)
    }
  )

  fun getItem(position: Int): T = sortedList.get(position)

  override fun getItemCount(): Int = sortedList.size()

  final override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder<*> =
    onCreateViewHolder(LayoutInflater.from(parent.context), parent, viewType)

  abstract fun onCreateViewHolder(
    inflater: LayoutInflater,
    parent: ViewGroup,
    viewType: Int
  ): ViewHolder<out T>

  final override fun onBindViewHolder(holder: ViewHolder<*>, position: Int) {
    @Suppress("UNCHECKED_CAST")
    (holder as ViewHolder<T>).bind(getItem(position))
  }

  fun addCallback(callback: Callback) {
    if (!callbacks.contains(callback)) callbacks.add(callback)
  }

  fun removeCallback(callback: Callback) {
    callbacks.remove(callback)
  }

  fun edit(): Editor = Editor()

  /**
   * Kötegelt módosító. A műveletek a [commit] híváskor, egyetlen
   * batched-update tranzakcióban érvényesülnek.
   */
  inner class Editor {
    private val toAdd = mutableListOf<T>()
    private var replacement: List<T>? = null
    private var replace = false

    fun add(item: T): Editor {
      toAdd.add(item)
      return this
    }

    fun add(items: List<T>): Editor {
      toAdd.addAll(items)
      return this
    }

    fun replaceAll(items: List<T>): Editor {
      replacement = ArrayList(items)
      replace = true
      return this
    }

    fun commit() {
      callbacks.forEach { it.onEditStarted() }
      sortedList.beginBatchedUpdates()
      try {
        if (replace) {
          sortedList.clear()
          sortedList.addAll(replacement ?: emptyList())
        }
        if (toAdd.isNotEmpty()) {
          sortedList.addAll(toAdd)
        }
      } finally {
        sortedList.endBatchedUpdates()
      }
      callbacks.forEach { it.onEditFinished() }
    }
  }

  /**
   * Típusonként eltérő rendezést felépítő builder. Egy adott [ViewModel]
   * altípushoz [Comparator]-t rendel; a [build]-elt comparator az első
   * illeszkedő szabályt alkalmazza.
   */
  class ComparatorBuilder<VM : ViewModel> {
    // Párhuzamos listák (Pair + star-projection helyett — a Kotlin 1.4.32
    // frontend belső hibát dobott a generikus star-projектált Pair-re).
    private val modelClasses = ArrayList<Class<*>>()
    private val modelComparators = ArrayList<Comparator<Any?>>()

    fun <M : VM> setOrderForModel(modelClass: Class<M>, comparator: Comparator<M>): ComparatorBuilder<VM> {
      modelClasses.add(modelClass)
      @Suppress("UNCHECKED_CAST")
      modelComparators.add(comparator as Comparator<Any?>)
      return this
    }

    fun build(): Comparator<VM> = object : Comparator<VM> {
      override fun compare(a: VM, b: VM): Int {
        for (i in modelClasses.indices) {
          val modelClass = modelClasses[i]
          if (modelClass.isInstance(a) && modelClass.isInstance(b)) {
            return modelComparators[i].compare(a, b)
          }
        }
        return 0
      }
    }
  }
}
