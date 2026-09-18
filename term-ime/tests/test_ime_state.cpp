#include <gtest/gtest.h>
#include "ime/engine.hpp"

class ImeStateTest : public ::testing::Test {
   protected:
    void SetUp() override {}
};

TEST_F(ImeStateTest, CandidateStruct) {
    Candidate cand;
    cand.text = U"你好";
    cand.code = "nihao";

    EXPECT_EQ(cand.text, U"你好");
    EXPECT_EQ(cand.code, "nihao");
}

TEST_F(ImeStateTest, ImeStateValues) {
    EXPECT_NE(ImeState::Inactive, ImeState::Composing);
    EXPECT_NE(ImeState::Composing, ImeState::Selecting);
    EXPECT_NE(ImeState::Inactive, ImeState::Selecting);
}

TEST_F(ImeStateTest, ImeModeValues) {
    EXPECT_NE(ImeMode::Chinese, ImeMode::English);
}

TEST_F(ImeStateTest, CandidateVector) {
    std::vector<Candidate> candidates;

    Candidate c1;
    c1.text = U"你好";
    c1.code = "nihao";
    candidates.push_back(c1);

    Candidate c2;
    c2.text = U"你号";
    c2.code = "nihao";
    candidates.push_back(c2);

    EXPECT_EQ(candidates.size(), 2);
    EXPECT_EQ(candidates[0].text, U"你好");
    EXPECT_EQ(candidates[1].text, U"你号");
}
