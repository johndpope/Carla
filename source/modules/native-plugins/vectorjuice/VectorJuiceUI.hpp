/*
 * Vector Juice Plugin
 * Copyright (C) 2014 Andre Sklenar <andre.sklenar@gmail.com>, www.juicelab.cz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the doc/GPL.txt file.
 */

#ifndef VECTORJUICEUI_HPP_INCLUDED
#define VECTORJUICEUI_HPP_INCLUDED

#include "DistrhoUI.hpp"

#include "Geometry.hpp"
#include "ImageAboutWindow.hpp"
#include "ImageButton.hpp"
#include "ImageKnob.hpp"
#include "ImageSlider.hpp"

#include "VectorJuiceArtwork.hpp"
#include "VectorJuicePlugin.hpp"

using DGL::Image;
using DGL::ImageAboutWindow;
using DGL::ImageButton;
using DGL::ImageKnob;
using DGL::ImageSlider;

START_NAMESPACE_DISTRHO

// -----------------------------------------------------------------------

class VectorJuiceUI : public UI,
                      public ImageButton::Callback,
                      public ImageKnob::Callback,
                      public ImageSlider::Callback
{
public:
    VectorJuiceUI();
    ~VectorJuiceUI() override;

protected:
    // -------------------------------------------------------------------
    // Information

    unsigned int d_getWidth() const noexcept override
    {
        return VectorJuiceArtwork::backgroundWidth;
    }

    unsigned int d_getHeight() const noexcept override
    {
        return VectorJuiceArtwork::backgroundHeight;
    }

    // -------------------------------------------------------------------
    // DSP Callbacks

    void d_parameterChanged(uint32_t index, float value) override;
    void d_programChanged(uint32_t index) override;

    // -------------------------------------------------------------------
    // Widget Callbacks

    void imageButtonClicked(ImageButton* button, int) override;
    void imageKnobDragStarted(ImageKnob* knob) override;
    void imageKnobDragFinished(ImageKnob* knob) override;
    void imageKnobValueChanged(ImageKnob* knob, float value) override;
    void imageSliderDragStarted(ImageSlider* slider) override;
    void imageSliderDragFinished(ImageSlider* slider) override;
    void imageSliderValueChanged(ImageSlider* slider, float value) override;

    void onDisplay() override;
    bool onMouse(int button, bool press, int x, int y) override;
    bool onMotion(int x, int y) override;

private:
    float paramX, paramY;

    Image fImgBackground;
    Image fImgRoundlet;
    Image fImgOrbit;
    Image fImgSubOrbit;
    ImageAboutWindow fAboutWindow;

    //knobs
    ImageKnob* fKnobOrbitSpeedX;
    ImageKnob* fKnobOrbitSpeedY;
    ImageKnob* fKnobOrbitSizeX;
    ImageKnob* fKnobOrbitSizeY;
    ImageKnob* fKnobSubOrbitSpeed;
    ImageKnob* fKnobSubOrbitSize;
    ImageKnob* fKnobSubOrbitSmooth;

    //sliders
    ImageSlider* fSliderOrbitWaveX;
    ImageSlider* fSliderOrbitWaveY;
    ImageSlider* fSliderOrbitPhaseX;
    ImageSlider* fSliderOrbitPhaseY;

    ImageButton* fButtonAbout;

    // needed for XY canvas handling
    bool fDragging;
    bool fDragValid;
    int  fLastX;
    int  fLastY;
    DGL::Rectangle<int> fCanvasArea;
    float orbitX, orbitY, subOrbitX, subOrbitY;
};

// -----------------------------------------------------------------------

END_NAMESPACE_DISTRHO

#endif // VECTORJUICEUI_HPP_INCLUDED