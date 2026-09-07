package main

import "math/rand"

type playbackOrder struct {
	count    int
	shuffle  bool
	order    []int
	position int
	rng      *rand.Rand
}

func newPlaybackOrder(count int, rng *rand.Rand) *playbackOrder {
	if rng == nil {
		rng = rand.New(rand.NewSource(1))
	}
	sequence := &playbackOrder{count: count, rng: rng}
	sequence.setSequential()
	return sequence
}

func (sequence *playbackOrder) setSequential() {
	sequence.shuffle = false
	sequence.order = make([]int, sequence.count)
	for index := range sequence.order {
		sequence.order[index] = index
	}
	sequence.position = 0
}

func (sequence *playbackOrder) SetShuffle(enabled bool, current int) {
	if sequence.count == 0 {
		sequence.shuffle = enabled
		sequence.order = nil
		sequence.position = 0
		return
	}
	if !enabled {
		sequence.setSequential()
		sequence.Select(current)
		return
	}
	sequence.shuffle = true
	sequence.order = sequence.randomPermutation()
	sequence.moveCurrentToFront(current)
	sequence.position = 0
}

func (sequence *playbackOrder) Select(current int) {
	if sequence.count == 0 {
		sequence.position = 0
		return
	}
	if current < 0 || current >= sequence.count {
		current = 0
	}
	for position, index := range sequence.order {
		if index == current {
			sequence.position = position
			return
		}
	}
	sequence.position = 0
}

func (sequence *playbackOrder) Next(current int) int {
	if sequence.count == 0 {
		return -1
	}
	sequence.Select(current)
	if !sequence.shuffle {
		sequence.position = (sequence.position + 1) % sequence.count
		return sequence.order[sequence.position]
	}
	nextPosition := sequence.position + 1
	if nextPosition >= sequence.count {
		sequence.order = sequence.randomPermutationAvoiding(current)
		nextPosition = 0
	}
	sequence.position = nextPosition
	return sequence.order[sequence.position]
}

func (sequence *playbackOrder) Previous(current int) int {
	if sequence.count == 0 {
		return -1
	}
	sequence.Select(current)
	sequence.position--
	if sequence.position < 0 {
		sequence.position = sequence.count - 1
	}
	return sequence.order[sequence.position]
}

func (sequence *playbackOrder) moveCurrentToFront(current int) {
	for position, index := range sequence.order {
		if index == current {
			sequence.order[0], sequence.order[position] = sequence.order[position], sequence.order[0]
			return
		}
	}
}

func (sequence *playbackOrder) randomPermutation() []int {
	order := make([]int, sequence.count)
	for index := range order {
		order[index] = index
	}
	sequence.rng.Shuffle(len(order), func(left, right int) {
		order[left], order[right] = order[right], order[left]
	})
	return order
}

func (sequence *playbackOrder) randomPermutationAvoiding(previous int) []int {
	order := sequence.randomPermutation()
	if len(order) > 1 && order[0] == previous {
		order[0], order[1] = order[1], order[0]
	}
	return order
}
