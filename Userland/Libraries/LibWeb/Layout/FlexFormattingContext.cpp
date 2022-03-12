/*
 * Copyright (c) 2021-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "InlineFormattingContext.h"
#include <AK/Function.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/FlexFormattingContext.h>
#include <LibWeb/Layout/InitialContainingBlock.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web::Layout {

static float get_pixel_size(FormattingState const& state, Box const& box, Optional<CSS::LengthPercentage> const& length_percentage)
{
    if (!length_percentage.has_value())
        return 0;
    auto inner_main_size = CSS::Length::make_px(state.get(*box.containing_block()).content_width);
    return length_percentage->resolved(box, inner_main_size).to_px(box);
}

static bool is_undefined_or_auto(Optional<CSS::LengthPercentage> const& length_percentage)
{
    if (!length_percentage.has_value())
        return true;
    return length_percentage->is_length() && length_percentage->length().is_auto();
}

FlexFormattingContext::FlexFormattingContext(FormattingState& state, Box const& flex_container, FormattingContext* parent)
    : FormattingContext(Type::Flex, state, flex_container, parent)
    , m_flex_container_state(m_state.get_mutable(flex_container))
    , m_flex_direction(flex_container.computed_values().flex_direction())
{
}

FlexFormattingContext::~FlexFormattingContext()
{
}

void FlexFormattingContext::run(Box const& run_box, LayoutMode)
{
    VERIFY(&run_box == &flex_container());

    // This implements https://www.w3.org/TR/css-flexbox-1/#layout-algorithm

    // FIXME: Implement reverse and ordering.

    // 1. Generate anonymous flex items
    generate_anonymous_flex_items();

    // 2. Determine the available main and cross space for the flex items
    float main_max_size = NumericLimits<float>::max();
    float main_min_size = 0;
    float cross_max_size = NumericLimits<float>::max();
    float cross_min_size = 0;
    bool main_is_constrained = false;
    bool cross_is_constrained = false;
    bool main_size_is_infinite = false;
    determine_available_main_and_cross_space(main_size_is_infinite, main_is_constrained, cross_is_constrained, main_min_size, main_max_size, cross_min_size, cross_max_size);

    // 3. Determine the flex base size and hypothetical main size of each item
    for (auto& flex_item : m_flex_items) {
        determine_flex_base_size_and_hypothetical_main_size(flex_item);
    }

    // 4. Determine the main size of the flex container
    determine_main_size_of_flex_container(main_is_constrained, main_size_is_infinite, m_available_space->main, main_min_size, main_max_size);

    // 5. Collect flex items into flex lines:
    // After this step no additional items are to be added to flex_lines or any of its items!
    collect_flex_items_into_flex_lines(m_available_space->main);

    // 6. Resolve the flexible lengths
    resolve_flexible_lengths(m_available_space->main);

    // Cross Size Determination
    // 7. Determine the hypothetical cross size of each item
    for (auto& flex_item : m_flex_items) {
        determine_hypothetical_cross_size_of_item(flex_item);
    }

    // 8. Calculate the cross size of each flex line.
    calculate_cross_size_of_each_flex_line(cross_min_size, cross_max_size);

    // 9. Handle 'align-content: stretch'.
    // FIXME: This

    // 10. Collapse visibility:collapse items.
    // FIXME: This

    // 11. Determine the used cross size of each flex item.
    determine_used_cross_size_of_each_flex_item();

    // 12. Distribute any remaining free space.
    distribute_any_remaining_free_space(m_available_space->main);

    // 13. Resolve cross-axis auto margins.
    // FIXME: This

    // 14. Align all flex items along the cross-axis
    align_all_flex_items_along_the_cross_axis();

    // 15. Determine the flex container’s used cross size:
    determine_flex_container_used_cross_size(cross_min_size, cross_max_size);

    // 16. Align all flex lines (per align-content)
    align_all_flex_lines();

    // AD-HOC: Layout the inside of all flex items.
    copy_dimensions_from_flex_items_to_boxes();
    for (auto& flex_item : m_flex_items) {
        auto independent_formatting_context = layout_inside(flex_item.box, LayoutMode::Default);
        independent_formatting_context->parent_context_did_dimension_child_root_box();
    }

    // FIXME: We run the "copy dimensions" step *again* here, in order to override any sizes
    //        assigned to the flex item by the "layout inside" step above. This is definitely not
    //        part of the spec, and simply covering up the fact that our inside layout currently
    //        mutates the height of BFC roots.
    copy_dimensions_from_flex_items_to_boxes();
}

void FlexFormattingContext::populate_specified_margins(FlexItem& item, CSS::FlexDirection flex_direction) const
{
    auto width_of_containing_block = m_state.get(*item.box.containing_block()).content_width;
    auto width_of_containing_block_as_length = CSS::Length::make_px(width_of_containing_block);
    // FIXME: This should also take reverse-ness into account
    if (flex_direction == CSS::FlexDirection::Row || flex_direction == CSS::FlexDirection::RowReverse) {
        item.margins.main_before = item.box.computed_values().margin().left.resolved(item.box, width_of_containing_block_as_length).to_px(item.box);
        item.margins.main_after = item.box.computed_values().margin().right.resolved(item.box, width_of_containing_block_as_length).to_px(item.box);
        item.margins.cross_before = item.box.computed_values().margin().top.resolved(item.box, width_of_containing_block_as_length).to_px(item.box);
        item.margins.cross_after = item.box.computed_values().margin().bottom.resolved(item.box, width_of_containing_block_as_length).to_px(item.box);
    } else {
        item.margins.main_before = item.box.computed_values().margin().top.resolved(item.box, width_of_containing_block_as_length).to_px(item.box);
        item.margins.main_after = item.box.computed_values().margin().bottom.resolved(item.box, width_of_containing_block_as_length).to_px(item.box);
        item.margins.cross_before = item.box.computed_values().margin().left.resolved(item.box, width_of_containing_block_as_length).to_px(item.box);
        item.margins.cross_after = item.box.computed_values().margin().right.resolved(item.box, width_of_containing_block_as_length).to_px(item.box);
    }
};

// https://www.w3.org/TR/css-flexbox-1/#flex-items
void FlexFormattingContext::generate_anonymous_flex_items()
{
    // More like, sift through the already generated items.
    // After this step no items are to be added or removed from flex_items!
    // It holds every item we need to consider and there should be nothing in the following
    // calculations that could change that.
    // This is particularly important since we take references to the items stored in flex_items
    // later, whose addresses won't be stable if we added or removed any items.
    flex_container().for_each_child_of_type<Box>([&](Box& child_box) {
        // Skip anonymous text runs that are only whitespace.
        if (child_box.is_anonymous() && !child_box.first_child_of_type<BlockContainer>()) {
            bool contains_only_white_space = true;
            child_box.for_each_in_inclusive_subtree_of_type<TextNode>([&contains_only_white_space](auto& text_node) {
                if (!text_node.dom_node().data().is_whitespace()) {
                    contains_only_white_space = false;
                    return IterationDecision::Break;
                }
                return IterationDecision::Continue;
            });
            if (contains_only_white_space)
                return IterationDecision::Continue;
        }

        // Skip any "out-of-flow" children
        if (child_box.is_out_of_flow(*this))
            return IterationDecision::Continue;

        child_box.set_flex_item(true);
        FlexItem flex_item = { child_box };
        populate_specified_margins(flex_item, m_flex_direction);
        m_flex_items.append(move(flex_item));
        return IterationDecision::Continue;
    });
}

bool FlexFormattingContext::has_definite_main_size(Box const& box) const
{
    return is_row_layout() ? box.has_definite_width() : box.has_definite_height();
}

float FlexFormattingContext::specified_main_size(Box const& box) const
{
    auto const& box_state = m_state.get(box);
    return is_row_layout() ? box_state.content_width : box_state.content_height;
}

float FlexFormattingContext::specified_cross_size(Box const& box) const
{
    auto const& box_state = m_state.get(box);
    return is_row_layout() ? box_state.content_height : box_state.content_width;
}

bool FlexFormattingContext::has_main_min_size(Box const& box) const
{
    auto value = is_row_layout() ? box.computed_values().min_width() : box.computed_values().min_height();
    return !is_undefined_or_auto(value);
}

bool FlexFormattingContext::has_cross_min_size(Box const& box) const
{
    auto value = is_row_layout() ? box.computed_values().min_height() : box.computed_values().min_width();
    return !is_undefined_or_auto(value);
}

bool FlexFormattingContext::has_definite_cross_size(Box const& box) const
{
    return is_row_layout() ? box.has_definite_height() : box.has_definite_width();
}

float FlexFormattingContext::specified_main_size_of_child_box(Box const& child_box) const
{
    auto main_size_of_parent = specified_main_size(flex_container());
    auto& value = is_row_layout() ? child_box.computed_values().width() : child_box.computed_values().height();
    if (!value.has_value())
        return 0;
    return value->resolved(child_box, CSS::Length::make_px(main_size_of_parent)).to_px(child_box);
}

float FlexFormattingContext::specified_main_min_size(Box const& box) const
{
    return is_row_layout()
        ? get_pixel_size(m_state, box, box.computed_values().min_width())
        : get_pixel_size(m_state, box, box.computed_values().min_height());
}

float FlexFormattingContext::specified_cross_min_size(Box const& box) const
{
    return is_row_layout()
        ? get_pixel_size(m_state, box, box.computed_values().min_height())
        : get_pixel_size(m_state, box, box.computed_values().min_width());
}

bool FlexFormattingContext::has_main_max_size(Box const& box) const
{
    return is_row_layout()
        ? !is_undefined_or_auto(box.computed_values().max_width())
        : !is_undefined_or_auto(box.computed_values().max_height());
}

bool FlexFormattingContext::has_cross_max_size(Box const& box) const
{
    return is_row_layout()
        ? !is_undefined_or_auto(box.computed_values().max_height())
        : !is_undefined_or_auto(box.computed_values().max_width());
}

float FlexFormattingContext::specified_main_max_size(Box const& box) const
{
    return is_row_layout()
        ? get_pixel_size(m_state, box, box.computed_values().max_width())
        : get_pixel_size(m_state, box, box.computed_values().max_height());
}

float FlexFormattingContext::specified_cross_max_size(Box const& box) const
{
    return is_row_layout()
        ? get_pixel_size(m_state, box, box.computed_values().max_height())
        : get_pixel_size(m_state, box, box.computed_values().max_width());
}

float FlexFormattingContext::calculated_main_size(Box const& box) const
{
    auto const& box_state = m_state.get(box);
    return is_row_layout() ? box_state.content_width : box_state.content_height;
}

bool FlexFormattingContext::is_cross_auto(Box const& box) const
{
    auto& cross_length = is_row_layout() ? box.computed_values().height() : box.computed_values().width();
    return cross_length.has_value() && cross_length->is_length() && cross_length->length().is_auto();
}

bool FlexFormattingContext::is_main_axis_margin_first_auto(Box const& box) const
{
    if (is_row_layout())
        return box.computed_values().margin().left.is_length() && box.computed_values().margin().left.length().is_auto();
    return box.computed_values().margin().top.is_length() && box.computed_values().margin().top.length().is_auto();
}

bool FlexFormattingContext::is_main_axis_margin_second_auto(Box const& box) const
{
    if (is_row_layout())
        return box.computed_values().margin().right.is_length() && box.computed_values().margin().right.length().is_auto();
    return box.computed_values().margin().bottom.is_length() && box.computed_values().margin().bottom.length().is_auto();
}

void FlexFormattingContext::set_main_size(Box const& box, float size)
{
    if (is_row_layout())
        m_state.get_mutable(box).content_width = size;
    else
        m_state.get_mutable(box).content_height = size;
}

void FlexFormattingContext::set_cross_size(Box const& box, float size)
{
    if (is_row_layout())
        m_state.get_mutable(box).content_height = size;
    else
        m_state.get_mutable(box).content_width = size;
}

void FlexFormattingContext::set_offset(Box const& box, float main_offset, float cross_offset)
{
    if (is_row_layout())
        m_state.get_mutable(box).offset = Gfx::FloatPoint { main_offset, cross_offset };
    else
        m_state.get_mutable(box).offset = Gfx::FloatPoint { cross_offset, main_offset };
}

void FlexFormattingContext::set_main_axis_first_margin(Box const& box, float margin)
{
    if (is_row_layout())
        m_state.get_mutable(box).margin_left = margin;
    else
        m_state.get_mutable(box).margin_top = margin;
}

void FlexFormattingContext::set_main_axis_second_margin(Box const& box, float margin)
{
    if (is_row_layout())
        m_state.get_mutable(box).margin_right = margin;
    else
        m_state.get_mutable(box).margin_bottom = margin;
}

float FlexFormattingContext::sum_of_margin_padding_border_in_main_axis(Box const& box) const
{
    auto const& box_state = m_state.get(box);

    if (is_row_layout()) {
        return box_state.margin_left + box_state.margin_right
            + box_state.padding_left + box_state.padding_right
            + box_state.border_left + box_state.border_right;
    } else {
        return box_state.margin_top + box_state.margin_bottom
            + box_state.padding_top + box_state.padding_bottom
            + box_state.border_top + box_state.border_bottom;
    }
}

// https://www.w3.org/TR/css-flexbox-1/#algo-available
void FlexFormattingContext::determine_available_main_and_cross_space(bool& main_size_is_infinite, bool& main_is_constrained, bool& cross_is_constrained, float& main_min_size, float& main_max_size, float& cross_min_size, float& cross_max_size)
{
    auto containing_block_effective_main_size = [&](Box const& box) {
        auto& containing_block = *box.containing_block();
        if (is_row_layout()) {
            if (containing_block.has_definite_width())
                return m_state.get(containing_block).content_width;
            main_size_is_infinite = true;
            return NumericLimits<float>::max();
        } else {
            if (containing_block.has_definite_height())
                return m_state.get(containing_block).content_height;
            main_size_is_infinite = true;
            return NumericLimits<float>::max();
        }
    };

    float main_available_space = 0;
    main_is_constrained = false;

    // For each dimension,
    //     if that dimension of the flex container’s content box is a definite size, use that;
    //     if that dimension of the flex container is being sized under a min or max-content constraint, the available space in that dimension is that constraint;
    //     otherwise, subtract the flex container’s margin, border, and padding from the space available to the flex container in that dimension and use that value. (This might result in an infinite value.)

    if (has_definite_main_size(flex_container())) {
        main_is_constrained = true;
        main_available_space = specified_main_size(flex_container());
    } else {
        if (has_main_max_size(flex_container())) {
            main_max_size = specified_main_max_size(flex_container());
            main_available_space = main_max_size;
            main_is_constrained = true;
        }
        if (has_main_min_size(flex_container())) {
            main_min_size = specified_main_min_size(flex_container());
            main_is_constrained = true;
        }

        if (!main_is_constrained) {
            auto available_main_size = containing_block_effective_main_size(flex_container());
            main_available_space = available_main_size - sum_of_margin_padding_border_in_main_axis(flex_container());
            if (flex_container().computed_values().flex_wrap() == CSS::FlexWrap::Wrap || flex_container().computed_values().flex_wrap() == CSS::FlexWrap::WrapReverse) {
                main_available_space = specified_main_size(*flex_container().containing_block());
                main_is_constrained = true;
            }
        }
    }

    float cross_available_space = 0;
    cross_is_constrained = false;

    if (has_definite_cross_size(flex_container())) {
        cross_available_space = specified_cross_size(flex_container());
    } else {
        if (has_cross_max_size(flex_container())) {
            cross_max_size = specified_cross_max_size(flex_container());
            cross_is_constrained = true;
        }
        if (has_cross_min_size(flex_container())) {
            cross_min_size = specified_cross_min_size(flex_container());
            cross_is_constrained = true;
        }

        // FIXME: Is this right? Probably not.
        if (!cross_is_constrained)
            cross_available_space = cross_max_size;
    }

    m_available_space = AvailableSpace { .main = main_available_space, .cross = cross_available_space };
}

float FlexFormattingContext::layout_for_maximum_main_size(Box const& box)
{
    bool main_constrained = false;
    if (is_row_layout()) {
        if (!is_undefined_or_auto(box.computed_values().width()) || !is_undefined_or_auto(box.computed_values().min_width())) {
            main_constrained = true;
        }
    } else {
        if (!is_undefined_or_auto(box.computed_values().height()) || !is_undefined_or_auto(box.computed_values().min_height())) {
            main_constrained = true;
        }
    }

    if (!main_constrained && box.children_are_inline()) {
        auto& block_container = verify_cast<BlockContainer>(box);
        BlockFormattingContext bfc(m_state, block_container, this);
        bfc.run(box, LayoutMode::Default);
        InlineFormattingContext ifc(m_state, block_container, bfc);

        if (is_row_layout()) {
            ifc.run(box, LayoutMode::OnlyRequiredLineBreaks);
            return m_state.get(box).content_width;
        } else {
            ifc.run(box, LayoutMode::AllPossibleLineBreaks);
            return m_state.get(box).content_height;
        }
    }
    if (is_row_layout()) {
        (void)layout_inside(box, LayoutMode::OnlyRequiredLineBreaks);
        return m_state.get(box).content_width;
    } else {
        return BlockFormattingContext::compute_theoretical_height(m_state, box);
    }
}

// https://www.w3.org/TR/css-flexbox-1/#algo-main-item
void FlexFormattingContext::determine_flex_base_size_and_hypothetical_main_size(FlexItem& flex_item)
{
    auto& child_box = flex_item.box;

    flex_item.flex_base_size = [&] {
        auto const& used_flex_basis = child_box.computed_values().flex_basis();

        // A. If the item has a definite used flex basis, that’s the flex base size.
        if (used_flex_basis.is_definite()) {
            auto specified_base_size = get_pixel_size(m_state, child_box, used_flex_basis.length_percentage.value());
            if (specified_base_size == 0)
                return calculated_main_size(flex_item.box);
            return specified_base_size;
        }

        // B. If the flex item has ...
        //    - an intrinsic aspect ratio,
        //    - a used flex basis of content, and
        //    - a definite cross size,
        if (flex_item.box.has_intrinsic_aspect_ratio()
            && used_flex_basis.type == CSS::FlexBasis::Content
            && has_definite_cross_size(child_box)) {
            TODO();
            // flex_base_size is calculated from definite cross size and intrinsic aspect ratio
        }

        // C. If the used flex basis is content or depends on its available space,
        //    and the flex container is being sized under a min-content or max-content constraint
        //    (e.g. when performing automatic table layout [CSS21]), size the item under that constraint.
        //    The flex base size is the item’s resulting main size.
        if (used_flex_basis.type == CSS::FlexBasis::Content
            // FIXME: && sized under min-content or max-content constraints
            && false) {
            TODO();
            // Size child_box under the constraints, flex_base_size is then the resulting main_size.
        }

        // D. Otherwise, if the used flex basis is content or depends on its available space,
        //    the available main size is infinite, and the flex item’s inline axis is parallel to the main axis,
        //    lay the item out using the rules for a box in an orthogonal flow [CSS3-WRITING-MODES].
        //    The flex base size is the item’s max-content main size.
        if (used_flex_basis.type == CSS::FlexBasis::Content
            // FIXME: && main_size is infinite && inline axis is parallel to the main axis
            && false && false) {
            TODO();
            // Use rules for a flex_container in orthogonal flow
        }

        // E. Otherwise, size the item into the available space using its used flex basis in place of its main size,
        //    treating a value of content as max-content. If a cross size is needed to determine the main size
        //    (e.g. when the flex item’s main size is in its block axis) and the flex item’s cross size is auto and not definite,
        //    in this calculation use fit-content as the flex item’s cross size.
        //    The flex base size is the item’s resulting main size.
        // FIXME: This is probably too naive.
        // FIXME: Care about FlexBasis::Auto
        if (has_definite_main_size(child_box))
            return specified_main_size_of_child_box(child_box);
        return layout_for_maximum_main_size(child_box);
    }();

    // The hypothetical main size is the item’s flex base size clamped according to its used min and max main sizes (and flooring the content box size at zero).
    auto clamp_min = has_main_min_size(child_box) ? specified_main_min_size(child_box) : 0;
    auto clamp_max = has_main_max_size(child_box) ? specified_main_max_size(child_box) : NumericLimits<float>::max();
    flex_item.hypothetical_main_size = clamp(flex_item.flex_base_size, clamp_min, clamp_max);
}

// https://www.w3.org/TR/css-flexbox-1/#algo-main-container
void FlexFormattingContext::determine_main_size_of_flex_container(bool const main_is_constrained, bool const main_size_is_infinite, float& main_available_size, float const main_min_size, float const main_max_size)
{
    if ((!main_is_constrained && main_size_is_infinite) || main_available_size == 0) {
        // Uses https://www.w3.org/TR/css-flexbox-1/#intrinsic-main-sizes
        // 9.9.1
        // 1.
        float largest_max_content_flex_fraction = 0;
        for (auto& flex_item : m_flex_items) {
            // FIXME: This needs some serious work.
            float max_content_contribution = calculated_main_size(flex_item.box);
            float max_content_flex_fraction = max_content_contribution - flex_item.flex_base_size;
            if (max_content_flex_fraction > 0) {
                max_content_flex_fraction /= max(flex_item.box.computed_values().flex_grow(), 1.0f);
            } else {
                max_content_flex_fraction /= max(flex_item.box.computed_values().flex_shrink(), 1.0f) * flex_item.flex_base_size;
            }
            flex_item.max_content_flex_fraction = max_content_flex_fraction;

            if (max_content_flex_fraction > largest_max_content_flex_fraction)
                largest_max_content_flex_fraction = max_content_flex_fraction;
        }

        // 2. Omitted
        // 3.
        float result = 0;
        for (auto& flex_item : m_flex_items) {
            auto product = 0;
            if (flex_item.max_content_flex_fraction > 0) {
                product = largest_max_content_flex_fraction * flex_item.box.computed_values().flex_grow();
            } else {
                product = largest_max_content_flex_fraction * max(flex_item.box.computed_values().flex_shrink(), 1.0f) * flex_item.flex_base_size;
            }
            result += flex_item.flex_base_size + product;
        }
        main_available_size = clamp(result, main_min_size, main_max_size);
    }
    set_main_size(flex_container(), main_available_size);
}

// https://www.w3.org/TR/css-flexbox-1/#algo-line-break
void FlexFormattingContext::collect_flex_items_into_flex_lines(float const main_available_size)
{
    // FIXME: Also support wrap-reverse

    // If the flex container is single-line, collect all the flex items into a single flex line.
    if (is_single_line()) {
        FlexLine line;
        for (auto& flex_item : m_flex_items) {
            line.items.append(&flex_item);
        }
        m_flex_lines.append(move(line));
        return;
    }

    // Otherwise, starting from the first uncollected item, collect consecutive items one by one
    // until the first time that the next collected item would not fit into the flex container’s inner main size
    // (or until a forced break is encountered, see §10 Fragmenting Flex Layout).
    // If the very first uncollected item wouldn't fit, collect just it into the line.

    // For this step, the size of a flex item is its outer hypothetical main size. (Note: This can be negative.)

    // Repeat until all flex items have been collected into flex lines.

    FlexLine line;
    float line_main_size = 0;
    for (auto& flex_item : m_flex_items) {
        if ((line_main_size + flex_item.hypothetical_main_size) > main_available_size) {
            m_flex_lines.append(move(line));
            line = {};
            line_main_size = 0;
        }
        line.items.append(&flex_item);
        line_main_size += flex_item.hypothetical_main_size;
    }
    m_flex_lines.append(move(line));
}

// https://www.w3.org/TR/css-flexbox-1/#resolve-flexible-lengths
void FlexFormattingContext::resolve_flexible_lengths(float const main_available_size)
{
    enum FlexFactor {
        FlexGrowFactor,
        FlexShrinkFactor
    };

    FlexFactor used_flex_factor;
    // 6.1. Determine used flex factor
    for (auto& flex_line : m_flex_lines) {
        size_t number_of_unfrozen_items_on_line = flex_line.items.size();

        float sum_of_hypothetical_main_sizes = 0;
        for (auto& flex_item : flex_line.items) {
            sum_of_hypothetical_main_sizes += flex_item->hypothetical_main_size;
        }
        if (sum_of_hypothetical_main_sizes < main_available_size)
            used_flex_factor = FlexFactor::FlexGrowFactor;
        else
            used_flex_factor = FlexFactor::FlexShrinkFactor;

        for (auto& flex_item : flex_line.items) {
            if (used_flex_factor == FlexFactor::FlexGrowFactor)
                flex_item->flex_factor = flex_item->box.computed_values().flex_grow();
            else if (used_flex_factor == FlexFactor::FlexShrinkFactor)
                flex_item->flex_factor = flex_item->box.computed_values().flex_shrink();
        }

        // 6.2. Size inflexible items
        auto freeze_item_setting_target_main_size_to_hypothetical_main_size = [&number_of_unfrozen_items_on_line](FlexItem& item) {
            item.target_main_size = item.hypothetical_main_size;
            number_of_unfrozen_items_on_line--;
            item.frozen = true;
        };
        for (auto& flex_item : flex_line.items) {
            if (flex_item->flex_factor.has_value() && flex_item->flex_factor.value() == 0) {
                freeze_item_setting_target_main_size_to_hypothetical_main_size(*flex_item);
            } else if (used_flex_factor == FlexFactor::FlexGrowFactor) {
                // FIXME: Spec doesn't include the == case, but we take a too basic approach to calculating the values used so this is appropriate
                if (flex_item->flex_base_size > flex_item->hypothetical_main_size) {
                    freeze_item_setting_target_main_size_to_hypothetical_main_size(*flex_item);
                }
            } else if (used_flex_factor == FlexFactor::FlexShrinkFactor) {
                if (flex_item->flex_base_size < flex_item->hypothetical_main_size) {
                    freeze_item_setting_target_main_size_to_hypothetical_main_size(*flex_item);
                }
            }
        }

        // 6.3. Calculate initial free space
        auto calculate_free_space = [&]() {
            float sum_of_items_on_line = 0;
            for (auto& flex_item : flex_line.items) {
                if (flex_item->frozen)
                    sum_of_items_on_line += flex_item->target_main_size;
                else
                    sum_of_items_on_line += flex_item->flex_base_size;
            }
            return main_available_size - sum_of_items_on_line;
        };

        float initial_free_space = calculate_free_space();

        // 6.4 Loop
        auto for_each_unfrozen_item = [&flex_line](auto callback) {
            for (auto& flex_item : flex_line.items) {
                if (!flex_item->frozen)
                    callback(flex_item);
            }
        };

        while (number_of_unfrozen_items_on_line > 0) {
            // b Calculate the remaining free space
            auto remaining_free_space = calculate_free_space();
            float sum_of_unfrozen_flex_items_flex_factors = 0;
            for_each_unfrozen_item([&](FlexItem* item) {
                sum_of_unfrozen_flex_items_flex_factors += item->flex_factor.value_or(1);
            });

            if (sum_of_unfrozen_flex_items_flex_factors < 1) {
                auto intermediate_free_space = initial_free_space * sum_of_unfrozen_flex_items_flex_factors;
                if (AK::abs(intermediate_free_space) < AK::abs(remaining_free_space))
                    remaining_free_space = intermediate_free_space;
            }

            // c Distribute free space proportional to the flex factors
            if (remaining_free_space != 0) {
                if (used_flex_factor == FlexFactor::FlexGrowFactor) {
                    float sum_of_flex_grow_factor_of_unfrozen_items = sum_of_unfrozen_flex_items_flex_factors;
                    for_each_unfrozen_item([&](FlexItem* flex_item) {
                        float ratio = flex_item->flex_factor.value_or(1) / sum_of_flex_grow_factor_of_unfrozen_items;
                        flex_item->target_main_size = flex_item->flex_base_size + (remaining_free_space * ratio);
                    });
                } else if (used_flex_factor == FlexFactor::FlexShrinkFactor) {
                    float sum_of_scaled_flex_shrink_factor_of_unfrozen_items = 0;
                    for_each_unfrozen_item([&](FlexItem* flex_item) {
                        flex_item->scaled_flex_shrink_factor = flex_item->flex_factor.value_or(1) * flex_item->flex_base_size;
                        sum_of_scaled_flex_shrink_factor_of_unfrozen_items += flex_item->scaled_flex_shrink_factor;
                    });

                    for_each_unfrozen_item([&](FlexItem* flex_item) {
                        float ratio = 1.0f;
                        if (sum_of_scaled_flex_shrink_factor_of_unfrozen_items != 0.0f)
                            ratio = flex_item->scaled_flex_shrink_factor / sum_of_scaled_flex_shrink_factor_of_unfrozen_items;
                        flex_item->target_main_size = flex_item->flex_base_size - (AK::abs(remaining_free_space) * ratio);
                    });
                }
            } else {
                // This isn't spec but makes sense.
                for_each_unfrozen_item([&](FlexItem* flex_item) {
                    flex_item->target_main_size = flex_item->flex_base_size;
                });
            }
            // d Fix min/max violations.
            float adjustments = 0.0f;
            for_each_unfrozen_item([&](FlexItem* item) {
                auto min_main = has_main_min_size(item->box)
                    ? specified_main_min_size(item->box)
                    : 0;
                auto max_main = has_main_max_size(item->box)
                    ? specified_main_max_size(item->box)
                    : NumericLimits<float>::max();

                float original_target_size = item->target_main_size;

                if (item->target_main_size < min_main) {
                    item->target_main_size = min_main;
                    item->is_min_violation = true;
                }

                if (item->target_main_size > max_main) {
                    item->target_main_size = max_main;
                    item->is_max_violation = true;
                }
                float delta = item->target_main_size - original_target_size;
                adjustments += delta;
            });
            // e Freeze over-flexed items
            float total_violation = adjustments;
            if (total_violation == 0) {
                for_each_unfrozen_item([&](FlexItem* item) {
                    --number_of_unfrozen_items_on_line;
                    item->frozen = true;
                });
            } else if (total_violation > 0) {
                for_each_unfrozen_item([&](FlexItem* item) {
                    if (item->is_min_violation) {
                        --number_of_unfrozen_items_on_line;
                        item->frozen = true;
                    }
                });
            } else if (total_violation < 0) {
                for_each_unfrozen_item([&](FlexItem* item) {
                    if (item->is_max_violation) {
                        --number_of_unfrozen_items_on_line;
                        item->frozen = true;
                    }
                });
            }
        }

        // 6.5.
        for (auto& flex_item : flex_line.items) {
            flex_item->main_size = flex_item->target_main_size;
        }
    }
}

// https://drafts.csswg.org/css-flexbox-1/#algo-cross-item
void FlexFormattingContext::determine_hypothetical_cross_size_of_item(FlexItem& item)
{
    // Determine the hypothetical cross size of each item by performing layout
    // as if it were an in-flow block-level box with the used main size
    // and the given available space, treating auto as fit-content.

    // If we have a definite cross size, this is easy! No need to perform layout, we can just use it as-is.
    if (has_definite_cross_size(item.box)) {
        auto const& cross_value = is_row_layout() ? item.box.computed_values().height() : item.box.computed_values().width();
        item.hypothetical_cross_size = cross_value->length().to_px(item.box);
        return;
    }

    // For indefinite cross sizes, we perform a throwaway layout and then measure it.
    auto throwaway_state = m_state;
    auto& tmp_container_state = throwaway_state.get_mutable(flex_container());
    tmp_container_state.content_width = is_row_layout() ? m_available_space->main : m_available_space->cross;

    VERIFY(item.box.containing_block() == &flex_container());
    if (auto independent_formatting_context = create_independent_formatting_context_if_needed(throwaway_state, item.box)) {
        independent_formatting_context->run(item.box, LayoutMode::Default);
    } else {
        // NOTE: Flex items should always create an independent formatting context!
        VERIFY_NOT_REACHED();
    }

    auto const& box_state = throwaway_state.get(item.box);
    if (is_row_layout()) {
        item.hypothetical_cross_size = BlockFormattingContext::compute_theoretical_height(throwaway_state, item.box);
    } else {
        item.hypothetical_cross_size = box_state.content_width;
    }
}

// https://www.w3.org/TR/css-flexbox-1/#algo-cross-line
void FlexFormattingContext::calculate_cross_size_of_each_flex_line(float const cross_min_size, float const cross_max_size)
{
    // If the flex container is single-line and has a definite cross size, the cross size of the flex line is the flex container’s inner cross size.
    if (is_single_line() && has_definite_cross_size(flex_container())) {
        m_flex_lines[0].cross_size = specified_cross_size(flex_container());
        return;
    }

    // Otherwise, for each flex line:
    for (auto& flex_line : m_flex_lines) {
        // FIXME: 1. Collect all the flex items whose inline-axis is parallel to the main-axis, whose align-self is baseline,
        //           and whose cross-axis margins are both non-auto. Find the largest of the distances between each item’s baseline
        //           and its hypothetical outer cross-start edge, and the largest of the distances between each item’s baseline
        //           and its hypothetical outer cross-end edge, and sum these two values.

        // FIXME: This isn't spec but makes sense here
        if (has_definite_cross_size(flex_container()) && flex_container().computed_values().align_items() == CSS::AlignItems::Stretch) {
            flex_line.cross_size = specified_cross_size(flex_container()) / m_flex_lines.size();
            continue;
        }

        // 2. Among all the items not collected by the previous step, find the largest outer hypothetical cross size.
        float largest_hypothetical_cross_size = 0;
        for (auto& flex_item : flex_line.items) {
            if (largest_hypothetical_cross_size < flex_item->hypothetical_cross_size_with_margins())
                largest_hypothetical_cross_size = flex_item->hypothetical_cross_size_with_margins();
        }

        // 3. The used cross-size of the flex line is the largest of the numbers found in the previous two steps and zero.
        flex_line.cross_size = max(0.0f, largest_hypothetical_cross_size);
    }

    // If the flex container is single-line, then clamp the line’s cross-size to be within the container’s computed min and max cross sizes.
    // Note that if CSS 2.1’s definition of min/max-width/height applied more generally, this behavior would fall out automatically.
    if (is_single_line())
        clamp(m_flex_lines[0].cross_size, cross_min_size, cross_max_size);
}

// https://www.w3.org/TR/css-flexbox-1/#algo-stretch
void FlexFormattingContext::determine_used_cross_size_of_each_flex_item()
{
    // FIXME: Get the alignment via "align-self" of the item (which accesses "align-items" of the parent if unset)
    for (auto& flex_line : m_flex_lines) {
        for (auto& flex_item : flex_line.items) {
            if (is_cross_auto(flex_item->box) && flex_container().computed_values().align_items() == CSS::AlignItems::Stretch) {
                flex_item->cross_size = flex_line.cross_size;
            } else {
                flex_item->cross_size = flex_item->hypothetical_cross_size;
            }
        }
    }
}

// https://www.w3.org/TR/css-flexbox-1/#algo-main-align
void FlexFormattingContext::distribute_any_remaining_free_space(float const main_available_size)
{
    for (auto& flex_line : m_flex_lines) {
        // 12.1.
        float used_main_space = 0;
        size_t auto_margins = 0;
        for (auto& flex_item : flex_line.items) {
            used_main_space += flex_item->main_size;
            if (is_main_axis_margin_first_auto(flex_item->box))
                ++auto_margins;
            if (is_main_axis_margin_second_auto(flex_item->box))
                ++auto_margins;
        }
        float remaining_free_space = main_available_size - used_main_space;
        if (remaining_free_space > 0) {
            float size_per_auto_margin = remaining_free_space / (float)auto_margins;
            for (auto& flex_item : flex_line.items) {
                if (is_main_axis_margin_first_auto(flex_item->box))
                    set_main_axis_first_margin(flex_item->box, size_per_auto_margin);
                if (is_main_axis_margin_second_auto(flex_item->box))
                    set_main_axis_second_margin(flex_item->box, size_per_auto_margin);
            }
        } else {
            for (auto& flex_item : flex_line.items) {
                if (is_main_axis_margin_first_auto(flex_item->box))
                    set_main_axis_first_margin(flex_item->box, 0);
                if (is_main_axis_margin_second_auto(flex_item->box))
                    set_main_axis_second_margin(flex_item->box, 0);
            }
        }

        // 12.2.
        float space_between_items = 0;
        float space_before_first_item = 0;
        auto number_of_items = flex_line.items.size();

        switch (flex_container().computed_values().justify_content()) {
        case CSS::JustifyContent::FlexStart:
            break;
        case CSS::JustifyContent::FlexEnd:
            space_before_first_item = main_available_size - used_main_space;
            break;
        case CSS::JustifyContent::Center:
            space_before_first_item = (main_available_size - used_main_space) / 2.0f;
            break;
        case CSS::JustifyContent::SpaceBetween:
            space_between_items = remaining_free_space / (number_of_items - 1);
            break;
        case CSS::JustifyContent::SpaceAround:
            space_between_items = remaining_free_space / number_of_items;
            space_before_first_item = space_between_items / 2.0f;
            break;
        }

        // FIXME: Support reverse
        float main_offset = space_before_first_item;
        for (auto& flex_item : flex_line.items) {
            flex_item->main_offset = main_offset;
            main_offset += flex_item->main_size + space_between_items;
        }
    }
}

void FlexFormattingContext::align_all_flex_items_along_the_cross_axis()
{
    // FIXME: Get the alignment via "align-self" of the item (which accesses "align-items" of the parent if unset)
    // FIXME: Take better care of margins
    float line_cross_offset = 0;
    for (auto& flex_line : m_flex_lines) {
        for (auto* flex_item : flex_line.items) {
            switch (flex_container().computed_values().align_items()) {
            case CSS::AlignItems::Baseline:
                // FIXME: Implement this
                //  Fallthrough
            case CSS::AlignItems::FlexStart:
            case CSS::AlignItems::Stretch:
                flex_item->cross_offset = line_cross_offset + flex_item->margins.cross_before;
                break;
            case CSS::AlignItems::FlexEnd:
                flex_item->cross_offset = line_cross_offset + flex_line.cross_size - flex_item->cross_size;
                break;
            case CSS::AlignItems::Center:
                flex_item->cross_offset = line_cross_offset + (flex_line.cross_size / 2.0f) - (flex_item->cross_size / 2.0f);
                break;
            default:
                break;
            }
        }

        line_cross_offset += flex_line.cross_size;
    }
}

// https://www.w3.org/TR/css-flexbox-1/#algo-cross-container
void FlexFormattingContext::determine_flex_container_used_cross_size(float const cross_min_size, float const cross_max_size)
{
    float cross_size = 0;
    if (has_definite_cross_size(flex_container())) {
        // Flex container has definite cross size: easy-peasy.
        cross_size = specified_cross_size(flex_container());
    } else {
        // Flex container has indefinite cross size.
        auto cross_size_value = is_row_layout() ? flex_container().computed_values().height() : flex_container().computed_values().width();
        if (!cross_size_value.has_value() || (cross_size_value->is_length() && cross_size_value->length().is_auto())) {
            // If a content-based cross size is needed, use the sum of the flex lines' cross sizes.
            float sum_of_flex_lines_cross_sizes = 0;
            for (auto& flex_line : m_flex_lines) {
                sum_of_flex_lines_cross_sizes += flex_line.cross_size;
            }
            cross_size = sum_of_flex_lines_cross_sizes;
        } else {
            // Otherwise, resolve the indefinite size at this point.
            cross_size = cross_size_value->resolved(flex_container(), CSS::Length::make_px(specified_cross_size(*flex_container().containing_block()))).to_px(flex_container());
        }
    }
    set_cross_size(flex_container(), clamp(cross_size, cross_min_size, cross_max_size));
}

// https://www.w3.org/TR/css-flexbox-1/#algo-line-align
void FlexFormattingContext::align_all_flex_lines()
{
    // FIXME: Support reverse
    if (is_single_line()) {
        // For single-line flex containers, we only need to center the line along the cross axis.
        auto& flex_line = m_flex_lines[0];
        float cross_size_of_flex_container = specified_cross_size(flex_container());
        for (auto* flex_item : flex_line.items)
            flex_item->cross_offset += (cross_size_of_flex_container / 2.0f) - (flex_line.cross_size / 2.0f);
    } else {
        // FIXME: Support align-content
    }
}

void FlexFormattingContext::copy_dimensions_from_flex_items_to_boxes()
{
    for (auto& flex_item : m_flex_items) {
        set_main_size(flex_item.box, flex_item.main_size);
        set_cross_size(flex_item.box, flex_item.cross_size);
        set_offset(flex_item.box, flex_item.main_offset, flex_item.cross_offset);
    }
}
}
