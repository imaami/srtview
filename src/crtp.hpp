/*
 * Curiously Recurring Template Pattern (CRTP) helper class
 */
#ifndef SRTVIEW_SRC_CRTP_HPP_
#define SRTVIEW_SRC_CRTP_HPP_

template <typename T, template <typename> typename C>
struct crtp
{
	constexpr T &impl() noexcept
	{
		return static_cast<T &>(*this);
	}

	constexpr T const &impl() const noexcept
	{
		return static_cast<T const &>(*this);
	}

private:
	constexpr crtp() noexcept {}
	friend C<T>;
};

#endif // SRTVIEW_SRC_CRTP_HPP_
