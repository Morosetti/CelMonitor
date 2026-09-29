package com.celmonitor.ui

import android.content.Context
import android.util.AttributeSet
import android.view.View
import android.widget.FrameLayout

/** Keeps its content at a fixed aspect ratio inside the parent (letterbox), so the image is never stretched. */
class AspectFrameLayout @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null) : FrameLayout(context, attrs) {
    private var aspect = 0.0 // width / height; 0 = fill parent

    fun setAspect(width: Int, height: Int) {
        val a = if (width > 0 && height > 0) width.toDouble() / height else 0.0
        if (a != aspect) { aspect = a; requestLayout() }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        if (aspect <= 0.0) return super.onMeasure(widthMeasureSpec, heightMeasureSpec)
        val maxW = View.MeasureSpec.getSize(widthMeasureSpec)
        val maxH = View.MeasureSpec.getSize(heightMeasureSpec)
        var w = maxW
        var h = (w / aspect).toInt()
        if (h > maxH) { h = maxH; w = (h * aspect).toInt() }
        super.onMeasure(
            View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY),
        )
    }
}
